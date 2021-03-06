/*
 *  This file is part of the LZPL: Linux ZFS Posix Layer
 *
 *  Copyright (c) 2010 Knowledge Quest Infotech Pvt. Ltd. 
 *  Produced at Knowledge Quest Infotech Pvt. Ltd. 
 *  Written by: Knowledge Quest Infotech Pvt. Ltd. 
 *              zfs@kqinfotech.com
 *
 *  This is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 */

#include <linux/fs.h>
#include <sys/vnode.h>
#include <spl-debug.h>
#include <sys/tsd.h>
#include <linux/writeback.h>
#include <lzfs_snap.h>
#include <linux/fsync_compat.h>
#include <linux/xattr.h>
#include <lzfs_xattr.h>

#ifdef SS_DEBUG_SUBSYS
#undef SS_DEBUG_SUBSYS
#endif

/*
 *  Log LZFS debug messages as the spl SS_USER2 subsystem.
 */
#define SS_DEBUG_SUBSYS SS_USER2

static int checkname(char *name) 
{
	if (strlen(name) >= MAXNAMELEN) {
		return ENAMETOOLONG;
	} else {
		return 0;
	}
}

static int lzfs_vnop_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	vnode_t *vnode = NULL;
	vattr_t vap;
	const struct cred *cred = get_current_cred();
	int err;

	SENTRY;
	vnode = LZFS_ITOV(inode);

	err = zfs_getattr(vnode, &vap, 0, (struct cred *) cred, NULL);
	if (err) {
		put_cred(cred);
		tsd_exit();
		SEXIT;
		return PTR_ERR(ERR_PTR(-err));
	}

	stat->dev   = vap.va_rdev;
	stat->rdev  = vap.va_rdev;
	stat->ino   = inode->i_ino;
	stat->mode  = vap.va_mode;
	stat->nlink = vap.va_nlink;
	stat->gid   = vap.va_gid;
	stat->uid   = vap.va_uid;
	stat->atime = vap.va_atime;
	stat->mtime = vap.va_mtime;
	stat->ctime = vap.va_ctime;
	stat->size  = i_size_read(inode);
	stat->blocks  = vap.va_nblocks;
	stat->blksize = (1 << inode->i_blkbits);
	// stat->blksize   = vap.va_blocksize;
	//stat->blocks    = stat->size >> inode->i_blkbits;
	put_cred(cred);
	tsd_exit();
	SEXIT;
	return 0;
}

static int
lzfs_vnop_create(struct inode *dir, struct dentry *dentry, int mode,
		 struct nameidata *nd)
{
	vnode_t *vp;
	vnode_t *dvp;
	vattr_t *vap;
	const struct cred *cred = get_current_cred();

	int err, se_err;

	SENTRY;
	err = checkname((char *)dentry->d_name.name);
	if(err)
		return -ENAMETOOLONG;
	vap = kmalloc(sizeof(vattr_t), GFP_KERNEL);
	ASSERT(vap != NULL);

	memset(vap, 0, sizeof(vap));

	vap->va_type = IFTOVT(mode);
	vap->va_mode = mode;
	vap->va_mask = AT_TYPE|AT_MODE;
	vap->va_uid = current_fsuid();
	vap->va_gid = current_fsgid();

	dvp = LZFS_ITOV(dir);

	err = zfs_create(dvp, (char *)dentry->d_name.name, vap, 0, mode,
			 &vp, (struct cred *)cred, 0, NULL, NULL);
	put_cred(cred);
	kfree(vap);
	if (err) {
		tsd_exit();
		SEXIT;
		return PTR_ERR(ERR_PTR(-err));
	}
	d_instantiate(dentry, LZFS_VTOI(vp));
	se_err = lzfs_init_security(dentry, dir);
	if(se_err) {
		tsd_exit();
		SEXIT;
		return se_err;
	}
	tsd_exit();
	SEXIT;
	return 0;
}

/* Read the directory. It uses the filldir function provided by Linux kernel.
 * 
 */

int
lzfs_vnop_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	vnode_t *vp;
	int eof, err;
	struct inode *inode = filp->f_path.dentry->d_inode;

	SENTRY;
	vp = LZFS_ITOV(inode);
	err = zfs_readdir(vp, dirent, NULL, &eof, NULL, 0, filldir, 
			&filp->f_pos);
	tsd_exit();
	SEXIT;
	if (err)
		return PTR_ERR(ERR_PTR(-err));
	return 0;
}


static struct dentry *
lzfs_vnop_lookup(struct inode * dir, struct dentry *dentry,
		 struct nameidata *nd)
{
	vnode_t *vp;
	vnode_t *dvp;
	int err;
	const struct cred *cred = get_current_cred();

	SENTRY;
	err = checkname((char *)dentry->d_name.name);
	if(err)
		return ((void * )-ENAMETOOLONG);
	dvp = LZFS_ITOV(dir);

	err = zfs_lookup(dvp, (char *)dentry->d_name.name, &vp, NULL, 0 , NULL, 
			(struct cred *) cred, NULL, NULL, NULL);
	put_cred(cred);
	tsd_exit();
	SEXIT;
	if (err) {
		if (err == ENOENT)
			return d_splice_alias(NULL, dentry);	
		else
			return ERR_PTR(-err);
	}

	return d_splice_alias(LZFS_VTOI(vp), dentry);
}

#define ZFS_LINK_MAX	UINT32_MAX

/* Add a hard link (new name) from dentry to inode pointed by old_dentry.
 */

static int
lzfs_vnop_link (struct dentry *old_dentry, struct inode * dir,
		struct dentry *dentry)
{
	vnode_t *tdvp;
	vnode_t *svp;
	struct inode *inode = old_dentry->d_inode;
	char *name = (char *)dentry->d_name.name;
	const struct cred *cred = get_current_cred();
	int err;

	SENTRY;
	/* Linux Kernel enforces a limit on number of hardlinks to a file. 
	 * struct kstat used in getattr uses unsigned int variable to store 
	 * hardlink count. ZFS does not restrict the number of hardlinks, 
	 * but we had to.
	 * */
	if (inode->i_nlink >= ZFS_LINK_MAX)
		return -EMLINK;

	err = checkname(name) || checkname((char *)old_dentry->d_name.name);
	if(err)
		return -ENAMETOOLONG;
	svp = LZFS_ITOV(inode);
	tdvp  = LZFS_ITOV(dir);

	inode->i_ctime = CURRENT_TIME_SEC;
	/* inode refernece counts are updated in zfs_inode_update
	 * inc_nlink(inode);
	 */
	atomic_inc(&inode->i_count);

	err = zfs_link(tdvp, svp, name, (struct cred *)cred, NULL, 0);
	put_cred(cred);
	if (err) {

		/* Decrement the link count and release the hold in error case.
		 */
	  
		/* inode refernece counts are updated in zfs_inode_update
		 * drop_nlink(inode);
		 */
		iput(inode);
		tsd_exit();
		SEXIT;
		return PTR_ERR(ERR_PTR(-err));
	}

	d_instantiate(dentry, LZFS_VTOI(svp));
	tsd_exit();
	SEXIT;
	return 0;
}

static int
lzfs_vnop_unlink(struct inode *dir, struct dentry *dentry)
{
	vnode_t *dvp;
	const struct cred *cred = get_current_cred();
	int err;

	SENTRY;
	dvp = LZFS_ITOV(dir);
	err = zfs_remove(dvp, (char *)dentry->d_name.name, 
			(struct cred *)cred, NULL, 0);
	put_cred(cred);
	tsd_exit();
	SEXIT;
	if (err)
		return PTR_ERR(ERR_PTR(-err));

	/* inode refernece counts are updated in zfs_inode_update
	 * drop_nlink(inode);
	 */
	return 0;
}

static int
lzfs_vnop_symlink (struct inode *dir, struct dentry *dentry,
		   const char *symname)
{
	vnode_t *dvp;
	vnode_t *vp;
	vattr_t *vap;
	const struct cred *cred = get_current_cred();
	int err, se_err;
	SENTRY;
	err = checkname((char *)dentry->d_name.name);
	if(err)
		return ENAMETOOLONG;
	vap = kmalloc(sizeof(vattr_t), GFP_KERNEL);

	ASSERT(vap != NULL);

	memset(vap, 0, sizeof(vap));
	dvp = LZFS_ITOV(dir);
	vap->va_type = VLNK; 
	vap->va_mode = (S_IRWXU | S_IRWXG | S_IRWXO);
	vap->va_mask = AT_TYPE|AT_MODE;
	vap->va_uid = cred->uid;
	vap->va_gid = cred->gid;

	err = zfs_symlink(dvp, (char *)dentry->d_name.name, vap, 
			(char *)symname, (struct cred *)cred , NULL, 0, &vp);
	kfree(vap);
	put_cred(cred);
	if (err) {
		tsd_exit();
		SEXIT;
		return PTR_ERR(ERR_PTR(-err));
	}
	d_instantiate(dentry, LZFS_VTOI(vp));
	se_err = lzfs_init_security(dentry, dir);
	if(se_err) {
 		tsd_exit();
		SEXIT;
		return se_err;
	}
	tsd_exit();
	SEXIT;
	return 0;
}

static int
lzfs_vnop_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	vnode_t *vp;
	vnode_t *dvp;
	vattr_t *vap;
	const struct cred *cred = get_current_cred();
	int err, se_err;
	SENTRY;
	err = checkname((char *)dentry->d_name.name);
	if(err)
		return -ENAMETOOLONG;
	vap = kmalloc(sizeof(vattr_t), GFP_KERNEL);
	ASSERT(vap != NULL);
	memset(vap, 0, sizeof(vap));
	vap->va_type = VDIR; 
	vap->va_mode = mode;
	vap->va_mask = AT_TYPE|AT_MODE;
	vap->va_uid = current_fsuid();
	vap->va_gid = current_fsgid();

	dvp = LZFS_ITOV(dir);
	err = zfs_mkdir(dvp, (char *)dentry->d_name.name, vap,
			&vp, (struct cred *) cred, NULL, 0, NULL);
	kfree(vap);
	put_cred(cred);
	if (err) {
		tsd_exit();
		SEXIT;
		return PTR_ERR(ERR_PTR(-err));
	}
	d_instantiate(dentry, LZFS_VTOI(vp));
	se_err = lzfs_init_security(dentry, dir);
	if(se_err) {
		tsd_exit();
		SEXIT;
		return se_err;
	}

	tsd_exit();
	SEXIT;
	return 0;
}

static int
lzfs_vnop_rmdir(struct inode * dir, struct dentry *dentry)
{
    vnode_t *dvp;
    const struct cred *cred = get_current_cred();
    int err;

    SENTRY;
    dvp = LZFS_ITOV(dir);
    err = zfs_rmdir(dvp, (char *)dentry->d_name.name, NULL, 
            (struct cred *) cred, NULL, 0);
    put_cred(cred);
	tsd_exit();
    SEXIT;
    if (err) 
    	return PTR_ERR(ERR_PTR(-err));
    return 0;
}

/* 
 * Create special node.
 * This code is very similar to create, just passing rdev in the vap and 
 * calling zfs_create.
 */

static int
lzfs_vnop_mknod(struct inode * dir, struct dentry *dentry, int mode,
		dev_t rdev)
{
	vnode_t *vp;
	vnode_t *dvp;
	vattr_t *vap;
	const struct cred *cred = get_current_cred();

	int err, se_err;
	SENTRY;
	vap = kmalloc(sizeof(vattr_t), GFP_KERNEL);
	ASSERT(vap != NULL);

	memset(vap, 0, sizeof(vap));

	vap->va_type = IFTOVT(mode); 
	vap->va_mode = mode;
	vap->va_mask = AT_TYPE|AT_MODE;
	vap->va_rdev = rdev;
	vap->va_uid = current_fsuid();
	vap->va_gid = current_fsgid();

/*
	printk(KERN_ERR "vap->va_type is %x, vap.vamode is %x orig mode %x dev %u\n",
	       vap->va_type, vap->va_mode, mode, vap->va_rdev);
*/

	dvp = LZFS_ITOV(dir);

	err = zfs_create(dvp, (char *)dentry->d_name.name, vap, 0, mode, 
			 &vp, (struct cred *)cred, 0, NULL, NULL);
	put_cred(cred);
	kfree(vap);
	if (err) {
		tsd_exit();
		SEXIT;
		return PTR_ERR(ERR_PTR(-err));
	}
	d_instantiate(dentry, LZFS_VTOI(vp));
	se_err = lzfs_init_security(dentry, dir);
	if(se_err) {
		tsd_exit();
		SEXIT;
		return se_err;
	}

	tsd_exit();
	SEXIT;
	return 0;
}

static int
lzfs_vnop_rename (struct inode * old_dir, struct dentry * old_dentry,
       struct inode * new_dir, struct dentry * new_dentry )
{
	vnode_t *sdvp = LZFS_ITOV(old_dir);
	vnode_t *tdvp = LZFS_ITOV(new_dir);
	const struct cred *cred = get_current_cred();
	int err;

	SENTRY;
	err = zfs_rename(sdvp, (char *)old_dentry->d_name.name, tdvp, 
			(char *) new_dentry->d_name.name, (struct cred *)cred, 
			NULL, 0);	
	put_cred(cred);
	tsd_exit();
	SEXIT;
	if (err)
		return PTR_ERR(ERR_PTR(-err));
	return 0;
}

int
lzfs_vnop_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	vnode_t *vp = LZFS_ITOV(inode);
	vattr_t *vap;
	int mask = iattr->ia_valid;
	const struct cred *cred = get_current_cred();
	int err;

	SENTRY;
	err = inode_change_ok(inode, iattr);
	if(err)
	    return err;

	vap = kmalloc(sizeof(vattr_t), GFP_KERNEL);
	ASSERT(vap != NULL);

	memset(vap, 0, sizeof(vap));
	if (mask & ATTR_MODE) {
		vap->va_mask |= AT_MODE;
		vap->va_mode = iattr->ia_mode;
	}
	if (mask & ATTR_UID) {
		vap->va_mask |= AT_UID;
		vap->va_uid = iattr->ia_uid;
	}
	if (mask & ATTR_GID) {
		vap->va_mask |= AT_GID;
		vap->va_gid = iattr->ia_gid;
	}
	vap->va_type = IFTOVT(inode->i_mode); 
	vap->va_mask |= AT_TYPE;
	if (mask & (ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)) {
		if (mask & ATTR_ATIME) {
			vap->va_mask |= AT_ATIME;       
			vap->va_atime = iattr->ia_atime;
		}
		if (mask & ATTR_MTIME) {
			vap->va_mask |= AT_MTIME; 
			vap->va_mtime = iattr->ia_mtime;
		}
		if (mask & ATTR_CTIME) {
			vap->va_mask |= AT_CTIME;
			vap->va_ctime = iattr->ia_ctime;
		}
	}

	if (mask & ATTR_SIZE) {
		/* truncate the inode, znode */
		vap->va_mask |= AT_SIZE;
		vap->va_size = iattr->ia_size;

		err = vmtruncate(inode, iattr->ia_size);
		if (err) {
			kfree(vap);
			put_cred(cred);
			tsd_exit();
			SEXIT;
			return err;
		}
	}

	err = zfs_setattr(vp, vap, 0, (struct cred *)cred, NULL);
	kfree(vap);
	put_cred(cred);
	tsd_exit();
	SEXIT;
	if (err)
		return PTR_ERR(ERR_PTR(-err));
	return 0;
}

int
lzfs_vnop_permission(struct inode *inode, int mask)
{
	return generic_permission(inode, mask, NULL);
}

static void lzfs_put_link(struct dentry *dentry, struct nameidata *nd, void *ptr)
{
    char *buf = nd_get_link(nd);
    if (!IS_ERR(buf)) {
        /* Free the char* */
        kfree(buf);
    }
}

static void *lzfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = dentry->d_inode;
	vnode_t *vp         = LZFS_ITOV(inode);
	const struct cred *cred = get_current_cred();
	size_t  len  = i_size_read(inode);
	char    *buf = NULL;
	struct iovec iov;
	uio_t uio;
	int err;

	SENTRY;

	if (NULL == (buf = kzalloc(len + 1, GFP_KERNEL))) {
		put_cred(cred);
		tsd_exit();
		SEXIT;
		return ERR_PTR(-ENOMEM);
	}

	bzero(&iov, sizeof(struct iovec));
	iov.iov_base = buf;
	iov.iov_len  = len;

	bzero(&uio, sizeof(uio_t));
	uio.uio_iov    = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_resid  = len;
	uio.uio_segflg = UIO_SYSSPACE;

	err = zfs_readlink(vp, &uio, (struct cred *)cred, NULL);
	if (err) {
		kfree(buf);
		buf = ERR_PTR(-err);
	} else
		buf[len] = '\0';

	nd_set_link(nd, buf);
	put_cred(cred);
	tsd_exit();
	SEXIT;
	return NULL;
}

#if 0
static int
lzfs_vnop_readlink(struct dentry *dentry, char __user *buf, int len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len};	
	vnode_t *vp = LZFS_ITOV(dentry->d_inode);
	const struct cred *cred = get_current_cred();
	uio_t uio;
	int err;

	SENTRY;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_resid = len;
	uio.uio_segflg = UIO_SYSSPACE;

//	printk("%s inode %ld\n", __FUNCTION__, dentry->d_inode->i_ino);
	err = zfs_readlink(vp, &uio, (struct cred *)cred, NULL);
	if (err)
		return PTR_ERR(ERR_PTR(-err));
	SEXIT;
/*
	printk("%s err %d ret %d\n", __FUNCTION__, err, 
	       (int)(len - uio.uio_resid));
*/
	
	return ((int) (len - uio.uio_resid));
}
#endif
LZFS_VNOP_FSYNC_HANDLER(lzfs_vnop_fsync)
{       
	int err = 0;
	vnode_t *vp = NULL;
	const struct cred *cred = get_current_cred();

	SENTRY;

	vp = LZFS_ITOV(filep->f_path.dentry->d_inode); 
	err = zfs_fsync(vp, datasync, (struct cred *)cred, NULL);

	put_cred(cred);
	SEXIT;
	return err;
}

/* XXX --> Internal function used by lzfs_vnop_readlink and lzfs_readpage
 *
 * Performs the read operation
 *
 * Explanation of parameters
 * 		buf        = buffer pointer to data
 * 		len        = length of the data
 * 		pos        = offest within file to wite
 * 		segment    = indicates whether buf pointer points to 
 * 		             user space buffer or kernel buffer.
 * */
static inline ssize_t lzfs_read(vnode_t *vp, const char *buf, ssize_t len, 
		loff_t pos, uio_seg_t segment)
{
	int          err;
	
	struct iovec iov = {
		.iov_base = (void *) buf,
		.iov_len  = len,
	};

	uio_t uio = {
		.uio_iov      = &iov,
		.uio_iovcnt   = 1,
		.uio_loffset  = (offset_t) pos,
		.uio_resid    = len,
		.uio_segflg   = segment,
	};

	const cred_t *cred = get_current_cred();

	err = zfs_read(vp, &uio, 0, (cred_t *) cred, NULL);
	put_cred(cred);
	if (unlikely(err))
		return -err;
	return (len - uio.uio_resid);
}

int copy_data(read_descriptor_t *desc, struct page *page, 
		unsigned long offset, unsigned long size)
{
	char *kaddr;
	unsigned long left, count = desc->count;

	if (size > count)
		size = count;

	kaddr = kmap(page);
	left  = copy_to_user(desc->arg.buf, kaddr + offset, size);
	kunmap(page);

	if (left) {
		size -= left;
		desc->error = -EFAULT;
	}

	desc->count    = count - size;
	desc->written += size;
	desc->arg.buf += size;
	return size;
}

ssize_t
lzfs_vnop_read (struct file *filep, char __user *buf, size_t len, loff_t *ppos)
{
	vnode_t *vp = NULL;
	loff_t isize;
	ssize_t size;

	/* PAGE CACHE SUPPORT VARIABLES */
	struct address_space *mapping = filep->f_mapping;
	struct inode *inode = mapping->host;
	struct file_ra_state *ra = &filep->f_ra;
	pgoff_t index;
	pgoff_t last_index;
	pgoff_t prev_index;
	unsigned long offset;      /* offset into pagecache page */
	unsigned int prev_offset;
	read_descriptor_t desc;

	SENTRY;
	vp  = LZFS_ITOV(inode);

	if (likely(!(vp->v_flag & VMMAPPED))) {
		/* file is not memory mmapped, pass read directly to ZFS */
		ssize_t rc;
		rc = lzfs_read(vp, buf, len, *ppos, UIO_USERSPACE);
		if (likely(rc > 0))
			*ppos += rc;
		tsd_exit();
		SEXIT;
		return rc;
	}

	desc.written = 0;
	desc.arg.buf = buf;
	desc.count   = len;
	desc.error   = 0;

	index = *ppos >> PAGE_CACHE_SHIFT;
	prev_index = ra->prev_pos >> PAGE_CACHE_SHIFT;
	prev_offset = ra->prev_pos & (PAGE_CACHE_SIZE - 1);
	last_index = (*ppos + len + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	offset = *ppos & ~PAGE_CACHE_MASK;

	while (1) {
		struct page *page;
		pgoff_t end_index;
		unsigned long nr, ret;

#if 0
		printk(KERN_INFO "desc.count=%ld, len=%ld, offset=%ld, pos=%lld\n", 
				desc.count, len, offset, *ppos);
#endif

		if (!desc.count)
			break;

		BUG_ON(desc.written > len);

		cond_resched();
		page = find_get_page(mapping, index);
		if (!page) {
			/* NORMAL CASE: DATA IS NOT CACHED */
			goto no_cached_page;
		}

		/* MMAPED DATA: DATA IS CACHED */
		if (!PageUptodate(page)) {
			/* WE ARE USING PAGE CACHE ONLY FOR MMAP, ONCE WE GET 
			 * LOCKED PAGE IT HAS TO BE UPTODATE */

			lock_page(page);

			if (!page->mapping) {
				/* truncate happened */
				unlock_page(page);
				page_cache_release(page);
				continue;
			}

			if (!PageUptodate(page)) {
				unlock_page(page);
				BUG();
			}
			unlock_page(page);
		}
		isize = i_size_read(inode);
		end_index = (isize - 1) >> PAGE_CACHE_SHIFT;
		if (unlikely(!isize || index > end_index)) {
			page_cache_release(page);
			goto out;
		}
		/* nr is the maximum number of bytes to copy from this page */
		nr = PAGE_CACHE_SIZE;
		if (index == end_index) {
			nr = ((isize - 1) & ~PAGE_CACHE_MASK) + 1;
			if (nr <= offset) {
				page_cache_release(page);
				goto out;
			}
		}
		nr = nr - offset;

        if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		mark_page_accessed(page);
		prev_index = index;

		ret = copy_data(&desc, page, offset, nr);
		*ppos += ret;
		offset += ret;
		index  += (offset >> PAGE_CACHE_SHIFT);
		offset &= ~PAGE_CACHE_MASK;
		prev_offset = offset;

		page_cache_release(page);

#if 0
		printk(KERN_ERR "%s: isize=%lld nr=%ld len=%ld offset=%ld desc.written=%ld desc.count=%ld pos=%lld.\n", 
				__FUNCTION__, isize, nr, len, offset, desc.written, desc.count, *ppos);
#endif

		if (*ppos >= isize) {
			ra->prev_pos = prev_index;
			ra->prev_pos <<= PAGE_CACHE_SHIFT;
			ra->prev_pos |= prev_offset;
			goto out;
		}

		if (ret == nr && desc.count)
			continue;

		goto out;
no_cached_page:
		isize = i_size_read(inode);
		if (*ppos >= isize)
			break;
		
		size = PAGE_CACHE_SIZE;
		if (desc.written + size > len)
			size = len - desc.written;

		if (offset + size > isize)
			size = isize - desc.written;

		ret = lzfs_read(vp, desc.arg.buf, size, *ppos, UIO_USERSPACE);
		if (unlikely(ret < 0)) {
			tsd_exit();
			SEXIT;
			return ret;
		}

		desc.count    = desc.count - ret;
		desc.written += ret;
		desc.arg.buf += ret;
		BUG_ON(desc.count < 0);

		*ppos += ret;
		index  += (offset >> PAGE_CACHE_SHIFT);
		offset = *ppos & ~PAGE_CACHE_MASK;
		prev_offset = offset;

#if 0
		printk(KERN_ERR "%s: isize=%lld size=%ld len=%ld offset=%ld written=%ld pos=%lld.\n", 
				__FUNCTION__, isize, size, len, offset, desc.written, *ppos);
#endif

		if (!desc.count)
			break; // goto out;
	}

out:
//	*ppos = ((loff_t)index << PAGE_CACHE_SHIFT) + offset;
	zfs_file_accessed(vp);
	tsd_exit();
	SEXIT;
	return ((ssize_t) (desc.written));
}

/* XXX --> Internal function used by lzfs_vnop_write and lzfs_writepage 
 *
 * Performs the write operation
 *
 * Explanation of parameters
 * 		file_flags = struct file *file->f_flags
 * 		buf        = buffer pointer to data
 * 		len        = length of the data
 * 		pos        = offest within file to wite
 * 		segment    = indicates whether buf pointer points to 
 * 		             user space buffer or kernel buffer.
 * */
static inline ssize_t lzfs_write(vnode_t *vp, unsigned int file_flags, 
		const char *buf, ssize_t len, loff_t pos, uio_seg_t segment)
{
	int          err;

	struct iovec iov = {
		.iov_base = (void *) buf,
		.iov_len  = len,
	};

	uio_t uio = {
		.uio_iov     = &iov,
		.uio_resid   = len,
		.uio_iovcnt  = 1,
		.uio_loffset = (offset_t)(pos),
		.uio_limit   = MAXOFFSET_T,
		.uio_segflg  = segment,
	};

	const cred_t *cred = get_current_cred();

	err = zfs_write(vp, &uio, file_flags, (cred_t *)cred, NULL);
	put_cred(cred);
	if (unlikely(err))
		return -err;
	return (len - uio.uio_resid);
}

ssize_t
lzfs_vnop_write (struct file *filep, const char __user *buf, size_t len, 
		 loff_t *ppos)
{
	vnode_t *vp = NULL;
	ssize_t rc;

	/* PAGE CACHE SUPPORT VARIABLES */
	struct address_space *mapping = filep->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t index;
	unsigned long written;
	unsigned long offset;

	const char *user_buf = buf;

	loff_t pos_append;

	SENTRY;
	vp = LZFS_ITOV(inode);

	rc = lzfs_write(vp, filep->f_flags, buf, len, *ppos, UIO_USERSPACE);
	if (unlikely(rc < 0)) {
		tsd_exit();
		SEXIT;
		return rc;
	}
	pos_append = *ppos;
	*ppos += rc;
	
	if (likely(!(vp->v_flag & VMMAPPED))) {
		/* file is not memory mmapped, pass write directly to ZFS */
		tsd_exit();
		SEXIT;
		return rc;
	}

	index = pos_append >> PAGE_CACHE_SHIFT;
	offset = pos_append & ~PAGE_CACHE_MASK;
	written = 0;

	while (1) {
		struct page *page = NULL;
		char *page_buf;
		ssize_t size;
		ssize_t tmp;

		if (written >= len)
			break;

		size = PAGE_CACHE_SIZE - offset;
		if (written + size > len) {
			size = len - written;
		}

		page = find_lock_page(mapping, index);
		if (likely(!page)) {
#if 0
			printk ("%s: Page not cached. ppos=%llu, pos_append=%llu, offset=%ld, "
					"len=%ld, size=%ld\n", __FUNCTION__, *ppos, pos_append, offset, 
					len, size);
#endif
			goto no_cached_page;
		}
#if 0
		printk ("%s: Page cached. ppos=%llu, pos_append=%llu, offset=%ld, len=%ld, "
				"size=%ld\n", __FUNCTION__, *ppos, pos_append, offset, len, size);
#endif

		if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

//		pagefault_disable();

		/* copy the data */
//		BUG_ON(!in_atomic());
		page_buf = kmap_atomic(page, KM_USER0);
		tmp = __copy_from_user_inatomic(page_buf + offset, user_buf, size);
		kunmap_atomic(page_buf, KM_USER0);
//		pagefault_enable();
		flush_dcache_page(page);
		if (tmp) {
			unlock_page(page);
			tsd_exit();
			SEXIT;
			return -EFAULT;
		}

		mark_page_accessed(page);

		SetPageUptodate(page);
		ClearPageError(page);
//		set_page_dirty(page);
		/* data is written to disk and then a page is modified 
		   i.e. page is not dirty */
//		page_clear_dirty(page);
		unlock_page(page);
		page_cache_release(page);
//		zfs_file_modified(vp);

//		balance_dirty_pages_ratelimited(mapping);

no_cached_page:
		user_buf += size;
		written  += size;
		pos_append += size;
		index    =  pos_append >> PAGE_CACHE_SHIFT;
		offset   =  0;
	}

	tsd_exit();
	SEXIT;
	return rc;
}

/* 
 * fops->open is not needed for default operations, but in case mmap is 
 * called on an opened file we need strcut file *, save it in vnode_t
 * */
static int lzfs_vnop_open(struct inode *inode, struct file *file)
{
	vnode_t *vp = NULL;
	int rc;

	if ((rc = generic_file_open(inode, file)) < 0)
		goto out;

	vp = LZFS_ITOV(inode);
	mutex_enter(&vp->v_lock);
	/* save struct file * */
	vp->v_file = file;
	mutex_exit(&vp->v_lock);
out:
	return rc;
}

const struct inode_operations zfs_symlink_inode_operations = {
    .readlink       = generic_readlink,
    .follow_link    = lzfs_follow_link,
    .put_link       = lzfs_put_link,
};

int lzfs_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct address_space *mapping = file->f_mapping;
	vnode_t *vp = LZFS_ITOV(mapping->host);
	int rc;

	rc = generic_file_mmap(file, vma);
	if (rc < 0)
		return rc;

	mutex_enter(&vp->v_lock);
	vp->v_flag |= VMMAPPED;
	mutex_exit(&vp->v_lock);
	return rc;
}

ssize_t
lzfs_vnop_aio_read(struct kiocb *iocb, const struct iovec *iov,
                unsigned long nr_segs, loff_t pos)
{
	ssize_t result;

	SENTRY;
	result = generic_file_aio_read(iocb, iov, nr_segs, pos);
	tsd_exit();
	SEXIT;
	return result;
}

static ssize_t lzfs_perform_write(struct file *file,
                                struct iov_iter *i, loff_t pos)
{
	struct address_space *mapping = file->f_mapping;
	long status = 0;
	ssize_t written = 0;
	vnode_t *vp = LZFS_ITOV(mapping->host);
       
	do {
		status = lzfs_write(vp, file->f_flags, 
				i->iov->iov_base, i->iov->iov_len, pos,
				UIO_USERSPACE);
		if (unlikely(status)) {
			break;
		}
		pos += status;
		written += status;
	} while (iov_iter_count(i));
	return written ? written : status;
}

ssize_t
lzfs_file_buffered_write(struct kiocb *iocb, const struct iovec *iov,
                unsigned long nr_segs, loff_t pos, loff_t *ppos,
                size_t count, ssize_t written)
{
	struct file *file = iocb->ki_filp;
	ssize_t status;
	struct iov_iter i;
       
	iov_iter_init(&i, iov, nr_segs, count, written);
	status = lzfs_perform_write(file, &i, pos);

	return status;
}

static ssize_t
__lzfs_vnop_aio_write(struct kiocb *iocb, const struct iovec *iov,
                                unsigned long nr_segs, loff_t *ppos)
{
	struct file *file = iocb->ki_filp;
	struct address_space * mapping = file->f_mapping;
	size_t ocount;          /* original count */
	size_t count;           /* after file limit checks */
	struct inode *inode = mapping->host;
	loff_t pos;
	ssize_t written;
	ssize_t err;

	ocount = 0;
	err = generic_segment_checks(iov, &nr_segs, &ocount, VERIFY_READ);
	if(err) {
		return err;
	}

	count = ocount;
	pos = *ppos;

	written = 0;

	err = generic_write_checks(file, &pos, &count, S_ISBLK(inode->i_mode));
	if(err) {
               goto out;
	}

	if(count == 0) {
		goto out;
	}
               
	file_update_time(file);
	written = lzfs_file_buffered_write(iocb, iov, nr_segs,
			pos, ppos, count, written);
out:
	return written ? written : err;
}

ssize_t
lzfs_vnop_aio_write(struct kiocb *iocb, const struct iovec *iov,
                unsigned long nr_segs, loff_t pos)
{

	ssize_t ret;

	SENTRY;
	BUG_ON(iocb->ki_pos != pos);
	ret = __lzfs_vnop_aio_write(iocb, iov, nr_segs,
				&iocb->ki_pos);
	tsd_exit();
	SEXIT;
	return ret;
}


const struct inode_operations zfs_inode_operations = {
	.getattr	= lzfs_vnop_getattr,
	.create         = lzfs_vnop_create,
	.link           = lzfs_vnop_link,
	.unlink         = lzfs_vnop_unlink,
	.symlink        = lzfs_vnop_symlink,
	.mkdir          = lzfs_vnop_mkdir,
	.rmdir          = lzfs_vnop_rmdir,
	.mknod          = lzfs_vnop_mknod,
	.rename         = lzfs_vnop_rename,
	.setattr        = lzfs_vnop_setattr,
	.permission     = lzfs_vnop_permission,
	.setxattr       = generic_setxattr,
	.getxattr       = generic_getxattr,
	.listxattr      = lzfs_listxattr,
	.removexattr    = lzfs_removexattr,
};

const struct file_operations zfs_file_operations = {
	.open               = lzfs_vnop_open,
    //.llseek           = generic_file_llseek,
	.read               = lzfs_vnop_read,
	.write              = lzfs_vnop_write,
	.readdir            = lzfs_vnop_readdir,
	.mmap               = lzfs_file_mmap,
    //.unlocked_ioctl   = lzfs_fop_ioctl,
	.fsync              = lzfs_vnop_fsync,
//	.aio_read           = lzfs_vnop_aio_read,
//	.aio_write          = lzfs_vnop_aio_write,
};

const struct inode_operations zfs_dir_inode_operations ={
	.create         = lzfs_vnop_create,
	.lookup         = lzfs_vnop_lookup,
	.link           = lzfs_vnop_link,
	.unlink         = lzfs_vnop_unlink,
	.symlink        = lzfs_vnop_symlink,
	.mkdir          = lzfs_vnop_mkdir,
	.rmdir          = lzfs_vnop_rmdir,
	.mknod          = lzfs_vnop_mknod,
	.rename         = lzfs_vnop_rename,
	.setattr        = lzfs_vnop_setattr,
	.permission     = lzfs_vnop_permission,
	.setxattr       = generic_setxattr,
	.getxattr       = generic_getxattr,
	.listxattr      = lzfs_listxattr,
	.removexattr    = lzfs_removexattr,
};

const struct file_operations zfs_dir_file_operations = {
//	.llseek         = generic_file_llseek,
//	.read           = generic_read_dir,
	.readdir        = lzfs_vnop_readdir,
//     .unlocked_ioctl = lzfs_fop_ioctl,
	.fsync          = lzfs_vnop_fsync,

};

static int lzfs_readpage(struct file *file, struct page *page)
{
    const cred_t *cred  = get_current_cred();
    struct inode *inode = NULL;
    loff_t i_size       = 0;
    loff_t offset       = 0;
    int err             = 0;
    char *buf           = NULL;
    vnode_t *vp         = NULL;
    unsigned long fillsize = 0;
	ssize_t rc;

    BUG_ON(!PageLocked(page));
//    printk(KERN_ERR "%s: ==>\n", __FUNCTION__);
    inode  = page->mapping->host;
    vp     = LZFS_ITOV(inode);
    i_size = i_size_read(inode);
    offset = page_offset(page);

    /* Is the page fully outside i_size? (truncate in progress) */
    if (unlikely(page->index >= (i_size + PAGE_CACHE_SIZE - 1) >>
                PAGE_CACHE_SHIFT)) {
        zero_user(page, 0, PAGE_CACHE_SIZE);
        printk(KERN_ERR "Read outside i_size - truncated?");
    	SetPageUptodate(page);
        goto out;
    }

    if (NULL == (buf = kmap(page))) {
        err = -ENOMEM;
        goto out;
    }

    if (offset < i_size) {
        i_size -= offset;
        fillsize = i_size > PAGE_CACHE_SIZE ? PAGE_CACHE_SIZE : i_size;

		rc = lzfs_read(vp, buf, fillsize, offset, UIO_SYSSPACE);
        if (unlikely(rc < 0)) {
            fillsize = 0;
            err = -EIO;
        }
    }

    if (fillsize < PAGE_CACHE_SIZE)
        memset(buf + fillsize, 0, PAGE_CACHE_SIZE - fillsize);

    if (!err) {
        SetPageUptodate(page);
		ClearPageError(page);
    	flush_dcache_page(page);
	} else {
		ClearPageUptodate(page);
        SetPageError(page);
	}

    kunmap(page);

out:
    unlock_page(page);
//    printk(KERN_ERR "%s: <==\n", __FUNCTION__);
    put_cred(cred);
    return err;
}

static int lzfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = NULL;
    loff_t i_size       = 0;
    loff_t offset       = 1;
    ssize_t len         = 0;
    int err             = 0;
    char *buf           = NULL;
    vnode_t *vp         = NULL;
    struct file *filep  = NULL;

//    printk(KERN_ERR "%s: ==>\n", __FUNCTION__);
    inode  = page->mapping->host;
    vp     = LZFS_ITOV(inode);
    filep  = vp->v_file;
    i_size = i_size_read(inode);
    offset = page_offset(page);

    BUG_ON(!PageLocked(page));
    BUG_ON(!filep);

    if (NULL == (buf = kmap(page))) {
        err = -ENOMEM;
        goto out;
    }

#if 0 
    for (i = 0; i < PAGE_CACHE_SIZE; i++) {
        if (buf[i] != 0) {
            printk(KERN_ERR "%c", buf[i]);
        }
    }
    printk(KERN_ERR "\n");
#endif

    /* WE ARE USING PAGE CACHE ONLY FOR MMAP, WE WOULD NEVER HAVE TO 
     * INCREASE THE SIZE OF A FILE IN writepage */
    len = PAGE_CACHE_SIZE;
    if ((offset + len) > i_size) {
        len = i_size & ~PAGE_CACHE_MASK;
    }

    /*
     * zfs_write handles the FAPPEND flag by changing the file offset 
     * to the end of the file, but the we are in writepage and data 
     * should be written to same offset that we provide to zfs_write.
     * */
	len = lzfs_write(vp, filep->f_flags & ~FAPPEND, buf, len, offset, UIO_SYSSPACE);
    if (unlikely(len < 0)) {
		ClearPageUptodate(page);
        SetPageError(page);
        err = -EIO;
	} else {
    	SetPageUptodate(page);
		ClearPageError(page);
	}
    kunmap(page);
out:
//    page_clear_dirty(page);
    unlock_page(page);
//    printk(KERN_ERR "%s: <==\n", __FUNCTION__);
    return err;
}


static ssize_t
lzfs_direct_IO(int rw, struct kiocb *iocb, const struct iovec *iov,
                        loff_t offset, unsigned long nr_segs)
{
        BUG_ON(1);
        return -1;
}


const struct address_space_operations zfs_address_space_operations = {
	.readpage = lzfs_readpage,
	.writepage = lzfs_writepage,
	.direct_IO = lzfs_direct_IO,
};

/* 
 * This file contains the entry point for all the inode operations.
 */

void
lzfs_set_inode_ops(struct inode *inode)
{
/*
	printk("%s inode number %ld i_mode %x\n", 
	       __FUNCTION__, inode->i_ino, inode->i_mode);
*/

	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
	    inode->i_op = &zfs_inode_operations;
	    inode->i_fop = &zfs_file_operations;
	    inode->i_mapping->a_ops = &zfs_address_space_operations;
	    break;
	case S_IFDIR:
	    inode->i_op = &zfs_dir_inode_operations;
	    inode->i_fop = &zfs_dir_file_operations;
	    break;
	case S_IFLNK:
	    inode->i_op = &zfs_symlink_inode_operations;
	    break;
	default:
	    inode->i_op = &zfs_inode_operations;
	    inode->i_fop = &zfs_file_operations;
	/*  
	    inode->i_op = &zfs_inode_operations;
	    init_special_inode(inode, inode->i_mode, inode->i_rdev);	
	*/
	    break;
    }
}
