/**
 *
 * Phantom OS
 *
 * Copyright (C) 2005-2010 Dmitry Zavalishin, dz@dz.ru
 *
 * Unix syscalls - file io
 *
 *
**/


#if HAVE_UNIX

#define DEBUG_MSG_PREFIX "funix"
#include <debug_ext.h>
#define debug_level_flow 0
#define debug_level_error 10
#define debug_level_info 10


#include <unix/uufile.h>
#include <unix/uuprocess.h>
#include <kernel/unix.h>
#include <kernel/init.h>
#include <sys/unistd.h>
#include <sys/fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <phantom_types.h>

#include "unix/fs_pipe.h"

//int uu_find_fd( uuprocess_t *u, uufile_t * f );



int usys_open( int *err, uuprocess_t *u, const char *name, int flags, int mode )
{
    SHOW_FLOW( 7, "open '%s'", name );

    uufile_t * f = uu_namei( name, u );
    if( f == 0 )
    {
        *err = ENOENT;
        return -1;
    }

    mode &= ~u->umask;

    hal_mutex_lock( &f->mutex );

    // TODO pass mode to open
    if( (f->fs == 0) || (f->fs->open == 0) )
        goto unlink;

    int wr = (flags & O_WRONLY) || (flags & O_RDWR);

    *err = f->fs->open( f, flags & O_CREAT, wr );
    if( *err )
        goto unlink;

    f->flags |= UU_FILE_FLAG_OPEN;

    if( !wr )
        f->flags |= UU_FILE_FLAG_RDONLY;

    int fd = uu_find_fd( u, f );

    if( fd < 0 )
    {
        *err = EMFILE;
        goto unlink;
    }

    hal_mutex_unlock( &f->mutex );
    return fd;

unlink:
    hal_mutex_unlock( &f->mutex );
    unlink_uufile( f );
    return -1;
}

int usys_creat( int *err, uuprocess_t *u, const char *name, int mode )
{
    return usys_open( err, u, name, O_CREAT|O_WRONLY|O_TRUNC, mode );
}

int usys_read(int *err, uuprocess_t *u, int fd, void *addr, int count )
{
    SHOW_FLOW(10, "rd %d", count);
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( (f->ops == 0) || (f->ops->read == 0) )
    {
        *err = ENXIO;
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    SHOW_FLOW(9, "do rd %d", count);
    int ret = f->ops->read( f, addr, count );
    hal_mutex_unlock( &f->mutex );

    if( ret < 0 ) *err = EIO;
    return ret;
}

int usys_write(int *err, uuprocess_t *u, int fd, const void *addr, int count )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if(f->flags & UU_FILE_FLAG_RDONLY)
    {
        *err = EBADF;
        return -1;
    }


    if( (f->ops == 0) || (f->ops->write == 0) )
    {
        *err = ENXIO;
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    int ret = f->ops->write( f, addr, count );
    hal_mutex_unlock( &f->mutex );

    if( ret < 0 ) *err = EIO;
    return ret;
}

int usys_close(int *err, uuprocess_t *u, int fd )
{
    CHECK_FD(fd);
    uufile_t *f = GETF(fd);

    hal_mutex_lock( &f->mutex );
    uufs_t *fs = f->fs;
    assert(fs->close != 0);
    *err = fs->close( f );

    //SHOW_FLOW( 8, "*err %d", *err );

    u->fd[fd] = 0;
    hal_mutex_unlock( &f->mutex );

    return *err ? -1 : 0;
}

int usys_lseek( int *err, uuprocess_t *u, int fd, int offset, int whence )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    hal_mutex_lock( &f->mutex );

    off_t pos = offset;

    switch(whence)
    {
    case SEEK_SET: break;
    case SEEK_CUR: pos += f->pos;
    case SEEK_END:
        {
            ssize_t size = f->ops->getsize( f );

            if(size < 0)
            {
                hal_mutex_unlock( &f->mutex );
                *err = ESPIPE;
                return -1;
            }

            pos += size;
        }
    }

    if(pos < 0)
    {
        hal_mutex_unlock( &f->mutex );
        *err = EINVAL;
        return -1;
    }

    f->pos = pos;

    if(f->ops->seek)
    {
        *err = f->ops->seek(f);
    }

    hal_mutex_unlock( &f->mutex );
    if(*err) return -1;
    return f->pos;
}





int usys_fchmod( int *err, uuprocess_t *u, int fd, int mode )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( f->ops->chmod == 0)
    {
        *err = ENOSYS;
        goto err;
    }

    if( (f->ops == 0) || (f->ops->chmod == 0) )
    {
        *err = ENXIO;
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->chmod( f, mode );
    hal_mutex_unlock( &f->mutex );
err:
    return *err ? -1 : 0;
}

int usys_fchown( int *err, uuprocess_t *u, int fd, int user, int grp )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( (f->ops == 0) || (f->ops->chown == 0) )
    {
        *err = ENXIO;
        return -1;
    }

    if( f->ops->chown == 0)
    {
        *err = ENOSYS;
        goto err;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->chown( f, user, grp );
    hal_mutex_unlock( &f->mutex );
err:
    return *err ? -1 : 0;
}



int usys_ioctl( int *err, uuprocess_t *u, int fd, int request, void *data, size_t len )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( (f->ops == 0) )
    {
        *err = ENXIO;
        return -1;
    }

    if( !f->ops->ioctl )
    {
        *err = ENOTTY;
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    int rc = f->ops->ioctl( f, err, request, data, len );
    hal_mutex_unlock( &f->mutex );

    return rc;
}


int usys_fcntl( int *err, uuprocess_t *u, int fd, int cmd, int arg )
{
    (void) u;
    SHOW_FLOW( 7, "fcntl %d", fd );

    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    hal_mutex_lock( &f->mutex );
    int rc = -1; // f->ops->ioctl( f, err, request, data, len );

    switch(cmd)
    {
    case F_GETFD:        /* Get file descriptor flags.  */
        rc = 0; // lower bit = noinherit, TODO
        break;

    case F_SETFD:        /* Set file descriptor flags.  */
        if( arg == 0 )
        {
            rc = 0;
            break;
        }
        goto ret_inval;

    case F_GETFL:        /* Get file status flags.  */
        //rc = 0; // TODO

        if(f->flags | UU_FILE_FLAG_RDONLY)
            rc = O_RDONLY;
        else
            rc = O_RDWR;

        break;

    case F_SETFL:        /* Set file status flags.  */
        goto ret_inval;

    default:
    ret_inval:
        *err = EINVAL;
        rc = -1;
    }

    hal_mutex_unlock( &f->mutex );

    return rc;
}


int usys_stat( int *err, uuprocess_t *u, const char *path, struct stat *data, int statlink )
{
    (void) u;
    (void) statlink;

    SHOW_FLOW( 7, "stat '%s'", path );

    uufile_t * f = uu_namei( path, u );
    if( f == 0 )
    {
        *err = ENOENT;
        return -1;
    }

    SHOW_FLOW( 10, "stat aft namei '%s'", path );

    if( (f->ops == 0) || (f->ops->stat == 0) )
    {
        *err = ENXIO; // or what?
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->stat( f, data );
    hal_mutex_unlock( &f->mutex );

    SHOW_FLOW( 10, "stat aft stat '%s'", path );

    unlink_uufile( f );
    SHOW_FLOW( 10, "stat aft unlink '%s'", path );
    return *err ? -1 : 0;
}


int usys_fstat( int *err, uuprocess_t *u, int fd, struct stat *data, int statlink )
{
    (void) statlink;

    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( (!f->ops) || (!f->ops->stat) )
    {
        *err = ENXIO; // or what?
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->stat( f, data );
    hal_mutex_unlock( &f->mutex );

    return *err ? -1 : 0;
}


int usys_truncate( int *err, uuprocess_t *u, const char *path, off_t length)
{
    (void) u;

    uufile_t * f = uu_namei( path, u );
    if( f == 0 )
    {
        *err = ENOENT;
        return -1;
    }

    if( (!f->ops) || ( !f->ops->setsize ) )
    {
        *err = ENXIO; // or what?
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->setsize( f, length );
    hal_mutex_unlock( &f->mutex );

    unlink_uufile( f );
    return *err ? -1 : 0;
}

int usys_ftruncate(int *err, uuprocess_t *u, int fd, off_t length)
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( (!f->ops) || ( !f->ops->setsize ) )
    {
        *err = ENXIO; // or what?
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->setsize( f, length );
    hal_mutex_unlock( &f->mutex );

    return *err ? -1 : 0;
}


// -----------------------------------------------------------------------
// Dirs
// -----------------------------------------------------------------------

int usys_chdir( int *err, uuprocess_t *u,  const char *in_path )
{
    char path[FS_MAX_PATH_LEN];

    if( uu_normalize_path( path, in_path, u ) )
        return ENOENT;

    SHOW_FLOW( 8, "in '%s' cd '%s' abs '%s'", in_path, u->cwd_path, path );

    uufile_t * f = uu_namei( path, u );
    if( f == 0 )
    {
        SHOW_ERROR( 1, "namei '%s' failed", path );
        *err = ENOENT;
        return -1;
    }

    if( !(f->flags & UU_FILE_FLAG_DIR) )
    {
        SHOW_ERROR( 1, " '%s' not dir", path );
        *err = ENOTDIR;
        goto err;
    }


#if 0
    if( f->ops->stat == 0)
    {
        *err = ENOTDIR;
        goto err;
    }

    struct stat sb;
    hal_mutex_lock( &f->mutex );
    *err = f->ops->stat( f, &sb );
    hal_mutex_unlock( &f->mutex );
    if( *err )
        goto err;

    if( sb.st_mode & _S_IFDIR)
    {
#endif
        if(u->cwd_file)
            unlink_uufile( u->cwd_file );

        u->cwd_file = f;
        //uu_absname( u->cwd_path, u->cwd_path, in_path );
        strlcpy( u->cwd_path, path, FS_MAX_PATH_LEN );
        SHOW_FLOW( 1, "cd to '%s'", path );
        return 0;
#if 0
    }

    *err = ENOTDIR;
#endif
err:
    unlink_uufile( f );
    return -1;
}

int usys_fchdir( int *err, uuprocess_t *u,  int fd )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( f->flags & UU_FILE_FLAG_DIR )
    {
        *err = ENOTDIR;
        goto err;
    }
// todo     hal_mutex_unlock( &f->mutex );

#if 0
    if( f->ops->stat == 0)
    {
        *err = ENOTDIR;
        goto err;
    }

    struct stat sb;
    *err = f->ops->stat( f, &sb );
    if( *err )
        goto err;

    if( sb.st_mode & _S_IFDIR)
    {
#endif
        if(u->cwd_file)
            unlink_uufile( u->cwd_file );
        u->cwd_file = copy_uufile( f );

        u->cwd_path[0] = 0;
        if(u->cwd_file->ops->getpath)
            u->cwd_file->ops->getpath( u->cwd_file, u->cwd_path, FS_MAX_PATH_LEN );


        return 0;
#if 0
    }

    *err = ENOTDIR;
#endif
err:
    return -1;
}


int usys_getcwd( int *err, uuprocess_t *u, char *buf, int bufsize )
{
    ssize_t len = strlen(u->cwd_path);
    SHOW_FLOW( 9, "bs %d len %d", bufsize, len );
    if(bufsize < len+1)
    {
        *err = EINVAL;
        return -1;
    }

    *buf = 0;
#if 1
    strlcpy( buf, u->cwd_path, bufsize );
    return len+1;
#else
    if(!u->cwd)
    {
        strlcpy( buf, "/", bufsize );
        return 0;
    }

    //size_t ret =
    hal_mutex_lock( &f->mutex );
    u->cwd->ops->getpath( u->cwd, buf, bufsize );
    hal_mutex_unlock( &f->mutex );

    return 0;
#endif
}



int usys_readdir(int *err, uuprocess_t *u, int fd, struct dirent *dirp )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    if( !(f->flags & UU_FILE_FLAG_DIR) )
    {
        *err = ENOTDIR;
        return -1;
    }

    if( !f->ops )
    {
        *err = ENXIO; // or what?
        return -1;
    }

    if( f->ops->readdir )
    {
        hal_mutex_lock( &f->mutex );
        *err = f->ops->readdir( f, dirp );
        hal_mutex_unlock( &f->mutex );
        if( *err == ENOENT )
        {
            *err = 0; // not err, just end of dir
            return 0;
        }
        if( *err  ) return -1;
        return 1;
    }

    if( !f->ops->read )
    {
        *err = ENXIO; // or what?
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    int len = f->ops->read( f, dirp, sizeof(struct dirent) );
    hal_mutex_unlock( &f->mutex );

    if( len == 0 )
        return 0;

    if( len == sizeof(struct dirent) )
        return 1;

    *err = EIO;
    return -1;
}


int usys_pipe(int *err, uuprocess_t *u, int *fds )
{
    uufile_t *f1;
    uufile_t *f2;

    pipefs_make_pipe( &f1, &f2 );

    int fd1 = uu_find_fd( u, f1 );
    int fd2 = uu_find_fd( u, f2 );

    if( (fd1 < 0) || (fd2 < 0)  )
    {
        unlink_uufile( f1 );
        unlink_uufile( f2 );
        *err = EMFILE;
        return -1;
    }

    fds[0] = fd1;
    fds[1] = fd2;

    return 0;
}


// -----------------------------------------------------------------------
// others
// -----------------------------------------------------------------------


int usys_rm( int *err, uuprocess_t *u, const char *name )
{
    (void) u;
    uufile_t * f = uu_namei( name, u );
    if( f == 0 )
    {
        *err = ENOENT;
        return -1;
    }

    if( 0 == f->ops->unlink)
    {
        *err = ENXIO;
        return -1;
    }

    hal_mutex_lock( &f->mutex );
    *err = f->ops->unlink( f );
    hal_mutex_unlock( &f->mutex );

    unlink_uufile( f );

    if( *err )
        return -1;

    return 0;
}

int usys_dup2(int *err, uuprocess_t *u, int src_fd, int dst_fd )
{
    // TODO lock fd[] access

    CHECK_FD_RANGE(dst_fd);
    CHECK_FD(src_fd);

    struct uufile *f = GETF(src_fd);

    if( u->fd[dst_fd] != 0)
        usys_close( err, u, dst_fd );

    link_uufile( f );
    u->fd[dst_fd] = f;

    return 0;
}

int usys_dup(int *err, uuprocess_t *u, int src_fd )
{
    // TODO lock fd[] access -- use pool!
    CHECK_FD(src_fd);

    struct uufile *f = GETF(src_fd);

    link_uufile( f );

    int fd = uu_find_fd( u, f  );

    if( fd < 0 )
    {
        unlink_uufile( f );
        *err = EMFILE;
        return -1;
    }

    return fd;
}

int usys_symlink(int *err, uuprocess_t *u, const char *src, const char *dst )
{
    (void) u;
    char rest[FS_MAX_PATH_LEN];

    uufs_t * fs = uu_findfs( src, rest );
    if( fs == 0 )
    {
        *err = ENOENT;
        return -1;
    }

    //hal_mutex_lock( &f->mutex );
    if(fs->symlink == 0)
        *err = ENXIO;
    else
        *err = fs->symlink( fs, rest, dst );
    //hal_mutex_unlock( &f->mutex );

    return *err ? -1 : 0;
}


int usys_mkdir( int *err, uuprocess_t *u, const char *path )
{
    (void) u;
    char rest[FS_MAX_PATH_LEN];

    uufs_t * fs = uu_findfs( path, rest );
    if( fs == 0 )
    {
        *err = ENOENT;
        return -1;
    }

    //hal_mutex_lock( &f->mutex );
    if(fs->mkdir == 0)
        *err = ENXIO;
    else
        *err = fs->mkdir( fs, rest );
    //hal_mutex_unlock( &f->mutex );

    return *err ? -1 : 0;
}


// -----------------------------------------------------------------------
// Phantom specific - properties
// -----------------------------------------------------------------------


errno_t usys_setproperty( int *err, uuprocess_t *u, int fd, const char *pName, const char *pValue )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    hal_mutex_lock( &f->mutex );
    if( 0 == f->ops->setproperty)
        *err = ENOTTY;
    else
        *err = f->ops->setproperty( f, pName, pValue );
    hal_mutex_unlock( &f->mutex );

    //*err = ENOTTY;

    return *err;
}

errno_t usys_getproperty( int *err, uuprocess_t *u, int fd, const char *pName, char *pValue, size_t vlen )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    hal_mutex_lock( &f->mutex );
    if( 0 == f->ops->getproperty)
        *err = ENOTTY;
    else
        *err = f->ops->getproperty( f, pName, pValue, vlen );
    hal_mutex_unlock( &f->mutex );

    //*err = ENOTTY;

    return *err;
}

errno_t usys_listproperties( int *err, uuprocess_t *u, int fd, int nProperty, char *buf, int buflen )
{
    CHECK_FD(fd);
    struct uufile *f = GETF(fd);

    hal_mutex_lock( &f->mutex );
    if( 0 == f->ops->listproperties)
        *err = ENOTTY;
    else
        *err = f->ops->listproperties( f, nProperty, buf, buflen );
    hal_mutex_unlock( &f->mutex );

    //*err = ENOTTY;

    return *err;
}





// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static hal_mutex_t fd_find_mutex;

static void init_fd_find_mutex(void)
{
    hal_mutex_init( &fd_find_mutex, "fd_find_mutex" );
}

INIT_ME( 0, init_fd_find_mutex, 0)


int uu_find_fd( uuprocess_t *u, uufile_t *f  )
{
    hal_mutex_lock( &fd_find_mutex );

    int i;
    for( i = 0; i < MAX_UU_FD; i++ )
    {
        if( u->fd[i] == 0 )
        {
            u->fd[i] = f;
            hal_mutex_unlock( &fd_find_mutex );
            return i;
        }
    }

    hal_mutex_unlock( &fd_find_mutex );
    return -1;
}

void uu_close_all( uuprocess_t *u  )
{
    int i;
    for( i = 0; i < MAX_UU_FD; i++ )
    {
        if( u->fd[i] )
        {
            SHOW_FLOW( 8, "close fd %d on exit of pid %d", i, u->pid );

            int err;
            if( usys_close( &err, u, i ) || err )
                SHOW_ERROR( 0, "Unable to close fd %d on proc death: %d", i, err );
        }
    }
}


#endif // HAVE_UNIX

