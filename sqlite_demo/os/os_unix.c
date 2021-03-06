
#include"sqlite_demo.h"

typedef struct UnixFile UnixFile;
struct UnixFile{
	struct SqlFile base; // 组合，类似 继承
	int fd;
	char* filename;
	UnixFile* next;
}*head=0; // 指向第一个打开的文件, 打开文件链表，使用头插法


int unixOpenFile(UnixFile* file, const char* filename, int flags);
UnixFile* unixAlreadyOpened(const char* filename);

int unixOpen(SqlVFS* vfs, const char* filename, SqlFile* ret, int flags);
int unixDelete(SqlVFS* vfs, const char* filename);
int unixAccess(SqlVFS* vfs, const char* filename, int flag, int *ret);
int unixSleep(SqlVFS* vfs, int micro_secs);
int unixCurrentTime(SqlVFS* vfs, int *ret);
int unixClose(SqlFile* file);
int unixRead(SqlFile* file, void* buf, int buf_sz, int offset);
int unixWrite(SqlFile* file, const void* content, int sz, int offset);
int unixTruncate(SqlFile* file, int sz);
int unixSync(SqlFile* file);
int unixFileSize(SqlFile* file, int *ret);

////////// unix的vfs操作：
int unixOpen(SqlVFS* vfs, const char* filename, SqlFile* ret, int flags)
{
	const static SqlIOMethods UNIX_METHODS={
		unixClose,
		unixRead,
		unixWrite,
		unixTruncate,
		unixSync,
		unixFileSize
	};	
	if(!ret  ||  !vfs  ||  !filename)
		return SQL_ERROR;
	
	// 检查文件是否已经打开
	UnixFile *dp=unixAlreadyOpened(filename);
	if(dp)
		return SQL_ERROR;
	
	UnixFile* p=(UnixFile*)ret; // SqlFile* 转换成 UnixFile*
	p->base.p_methods=&UNIX_METHODS;
	
	// 以指定flags打开文件
	if(unixOpenFile(p, filename, flags))
		return SQL_ERROR;
	
	p->next=head;
	head=p;
	return SQL_OK;
}

int unixDelete(SqlVFS* vfs, const char* filename)
{
	if(!filename)
		return SQL_ERROR;
	UnixFile* dp=unixAlreadyOpened(filename);
	if(dp)
		unixClose((SqlFile*)dp);
	return unlink(filename);
}

/*调用进程检查文件权限: 
F_OK检查文件是否存在， 
X_OK R_OK W_OK检查文件是否可对应操作，
成功返回0，否则返回-1*/
/*使用调用进程的真实UID和GID检查，对于set-user-id的程序，也能检查调用用户的实际权限*/
int unixAccess(SqlVFS* vfs, const char* filename, int flag, int *ret)
{
	if(!vfs  ||  !filename  ||  !ret)
		return SQL_ERROR;
	*ret=access(filename, flag);
	return SQL_OK;
}

int unixSleep(SqlVFS* vfs, int micro_secs) // 微妙
{
	static const int T=1000000;
	sleep(micro_secs/T);
	usleep(micro_secs % T);
	return SQL_OK;
}

int unixCurrentTime(SqlVFS* vfs, int *ret)
{
	time(ret);
	return SQL_OK;
}

//////////////// unix的 SqlFile 操作
int unixClose(SqlFile* file)
{
	if(!file)
		return SQL_ERROR;
	UnixFile* dp=(UnixFile*)file;
	// 将file从打开文件链表中删除
	UnixFile** p=&head;
	for(; *p!=dp  &&  *p; p=&((*p)->next))
	{}
	if(*p==0)
		return SQL_ERROR;
	*p=dp->next; // 修改指向file的指针的内容，为file的后一个节点地址
	
	int ret=close(dp->fd);
	free(dp->filename);
	return ret;
}

int unixRead(SqlFile* file, void* buf, int buf_sz, int offset)
{
	if(!file  ||  !buf)
		return SQL_ERROR;
	UnixFile* p=(UnixFile*)file;
	int off=lseek(p->fd, offset, SEEK_SET);
	if(off!=offset)
		return SQL_ERROR;
	return read(p->fd, buf, buf_sz);
}

int unixWrite(SqlFile* file, const void* content, int sz, int offset)
{
	if(!file)
		return SQL_ERROR;
	UnixFile* p=(UnixFile*)file;
	int off=lseek(p->fd, offset, SEEK_SET);
	if(off!=offset)
		return SQL_ERROR;
	return write(p->fd, content, sz);
}

int unixTruncate(SqlFile* file, int sz)
{
	if(!file)
		return SQL_ERROR;
	UnixFile* p=(UnixFile*)file;
	return ftruncate(p->fd, sz);
}

int unixSync(SqlFile* file)
{
	if(!file)
		return SQL_ERROR;
	UnixFile* p=(UnixFile*)file;
	//return syncfs(p->fd); 
	return fsync(p->fd);
}

int unixFileSize(SqlFile* file, int *ret)
{
	if(!file  ||  !ret)
		return SQL_ERROR;
	UnixFile* p=(UnixFile*)file;
	struct stat st;
	if(fstat(p->fd, &st)<0)
		return SQL_ERROR;
	*ret=st.st_size; // 文件的字节大小
	return SQL_OK;
}

//////////////////////// unix系统调用
int unixOpenFile(UnixFile* file, const char* filename, int flags)
{
	// 0744
	int fd=open(filename, flags, SQL_DEFAULT_FILE_CREATE_MODE);
	if(fd<0)
		return SQL_ERROR;
	file->fd=fd;
	int n=strlen(filename);
	file->filename=(char*)malloc(n+1);
	memcpy(file->filename, filename, n);
	file->filename[n]=0;
	return SQL_OK;
}

UnixFile* unixAlreadyOpened(const char* filename)
{
	UnixFile* p=head;
	for(; p&&  strcmp(p->filename, filename); p=p->next)
	{}
	return p;
}

SqlVFS* unixGetOS()
{
	// unix平台默认的vfs
	static struct SqlVFS UNIX_OS={
		sizeof(UnixFile),
		200, 
		"unix",
		0,
		
		unixOpen,
		unixDelete,
		unixAccess,
		unixSleep,
		unixCurrentTime
	};
	
	return &UNIX_OS;
}
