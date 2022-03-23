/*---------------------------------------------------------------------------------

	$Id: main.cpp,v 1.13 2008-12-02 20:21:20 dovoto Exp $

	Simple console print demo
	-- dovoto


---------------------------------------------------------------------------------*/
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <time.h>
            
#include "sqlite3.h"

// Heap. Avoid using too much stack on the NDS. Default stack is on the DTCM (16 kB)
static PrintConsole topScreen;
static PrintConsole bottomScreen;

char sql_query[1024];

// Implement FAT filesystem handler (And NDS)
// Taken and ported from ESP32 SQLite implementation
#ifndef SQLITE_ESP32VFS_BUFFERSZ
# define SQLITE_ESP32VFS_BUFFERSZ 8192
#endif
#define MAXPATHNAME 100

typedef struct NDSFile NDSFile;
struct NDSFile {
  sqlite3_file base;              /* Base class. Must be first. */
  FILE *fp;                       /* File descriptor */

  char *aBuffer;                  /* Pointer to malloc'd buffer */
  int nBuffer;                    /* Valid bytes of data in zBuffer */
  sqlite3_int64 iBufferOfst;      /* Offset in file of zBuffer[0] */
};



static int NDSDirectWrite(
  NDSFile *p,                    /* File handle */
  const void *zBuf,               /* Buffer containing data to write */
  int iAmt,                       /* Size of data to write in bytes */
  sqlite_int64 iOfst              /* File offset to write to */
){
  off_t ofst;                     /* Return value from lseek() */
  size_t nWrite;                  /* Return value from write() */

  //Serial.println("fn: DirectWrite:");

  ofst = fseek(p->fp, iOfst, SEEK_SET); //lseek(p->fd, iOfst, SEEK_SET);
  if( ofst != 0 ){
    //Serial.println("Seek error");
    return SQLITE_IOERR_WRITE;
  }

  nWrite = fwrite(zBuf, 1, iAmt, p->fp); // write(p->fd, zBuf, iAmt);
  if( nWrite!=iAmt ){
    //Serial.println("Write error");
    return SQLITE_IOERR_WRITE;
  }

  //Serial.println("fn:DirectWrite:Success");

  return SQLITE_OK;
}

static int NDSFlushBuffer(NDSFile *p){
  int rc = SQLITE_OK;
  //Serial.println("fn: FlushBuffer");
  if( p->nBuffer ){
    rc = NDSDirectWrite(p, p->aBuffer, p->nBuffer, p->iBufferOfst);
    p->nBuffer = 0;
  }
  //Serial.println("fn:FlushBuffer:Success");
  return rc;
}

/*
** Close a file.
*/
static int NDSClose(sqlite3_file *pFile){
  int rc;
  //Serial.println("fn: Close");
  NDSFile *p = (NDSFile*)pFile;
  rc = NDSFlushBuffer(p);
  sqlite3_free(p->aBuffer);
  fclose(p->fp);
  //Serial.println("fn:Close:Success");
  return rc;
}


/*
** Read data from a file.
*/
static int NDSRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
      //Serial.println("fn: Read");
  NDSFile *p = (NDSFile*)pFile;
  off_t ofst;                     /* Return value from lseek() */
  int nRead;                      /* Return value from read() */
  int rc;                         /* Return code from ESP32FlushBuffer() */

  /* Flush any data in the write buffer to disk in case this operation
  ** is trying to read data the file-region currently cached in the buffer.
  ** It would be possible to detect this case and possibly save an 
  ** unnecessary write here, but in practice SQLite will rarely read from
  ** a journal file when there is data cached in the write-buffer.
  */
  rc = NDSFlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  ofst = fseek(p->fp, iOfst, SEEK_SET); //lseek(p->fd, iOfst, SEEK_SET);
  //if( ofst != 0 ){
  //  return SQLITE_IOERR_READ;
  //}
  nRead = fread(zBuf, 1, iAmt, p->fp); // read(p->fd, zBuf, iAmt);

  if( nRead==iAmt ){
    //Serial.println("fn:Read:Success");
    return SQLITE_OK;
  }else if( nRead>=0 ){
    return SQLITE_IOERR_SHORT_READ;
  }

  return SQLITE_IOERR_READ;
}

/*
** Write data to a crash-file.
*/
static int NDSWrite(
  sqlite3_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
      //Serial.println("fn: Write");
  NDSFile *p = (NDSFile*)pFile;
  
  if( p->aBuffer ){
    char *z = (char *)zBuf;       /* Pointer to remaining data to write */
    int n = iAmt;                 /* Number of bytes at z */
    sqlite3_int64 i = iOfst;      /* File offset to write to */

    while( n>0 ){
      int nCopy;                  /* Number of bytes to copy into buffer */

      /* If the buffer is full, or if this data is not being written directly
      ** following the data already buffered, flush the buffer. Flushing
      ** the buffer is a no-op if it is empty.  
      */
      if( p->nBuffer==SQLITE_ESP32VFS_BUFFERSZ || p->iBufferOfst+p->nBuffer!=i ){
        int rc = NDSFlushBuffer(p);
        if( rc!=SQLITE_OK ){
          return rc;
        }
      }
      assert( p->nBuffer==0 || p->iBufferOfst+p->nBuffer==i );
      p->iBufferOfst = i - p->nBuffer;

      /* Copy as much data as possible into the buffer. */
      nCopy = SQLITE_ESP32VFS_BUFFERSZ - p->nBuffer;
      if( nCopy>n ){
        nCopy = n;
      }
      memcpy(&p->aBuffer[p->nBuffer], z, nCopy);
      p->nBuffer += nCopy;

      n -= nCopy;
      i += nCopy;
      z += nCopy;
    }
  }else{
    return NDSDirectWrite(p, zBuf, iAmt, iOfst);
  }
  //Serial.println("fn:Write:Success");

  return SQLITE_OK;
}

/*
** Truncate a file. This is a no-op for this VFS (see header comments at
** the top of the file).
*/
static int NDSTruncate(sqlite3_file *pFile, sqlite_int64 size){
      //Serial.println("fn: Truncate");
#if 0
  if( ftruncate(((ESP32File *)pFile)->fd, size) ) return SQLITE_IOERR_TRUNCATE;
#endif
  //Serial.println("fn:Truncate:Success");
  return SQLITE_OK;
}

/*
** Sync the contents of the file to the persistent media.
*/
static int NDSSync(sqlite3_file *pFile, int flags){
      //Serial.println("fn: Sync");
  NDSFile *p = (NDSFile*)pFile;
  int rc;

  rc = NDSFlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  rc = fflush(p->fp);
  if (rc != 0)
    return SQLITE_IOERR_FSYNC;
  // rc = fsync(fileno(p->fp));
  //if (rc == 0)
    //Serial.println("fn:Sync:Success");
  return SQLITE_OK; // ignore fsync return value // (rc==0 ? SQLITE_OK : SQLITE_IOERR_FSYNC);
}

/*
** Write the size of the file in bytes to *pSize.
*/
static int NDSFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
      //Serial.println("fn: FileSize");
  NDSFile *p = (NDSFile*)pFile;
  int rc;                         /* Return code from fstat() call */

  /* Flush the contents of the buffer to disk. As with the flush in the
  ** ESP32Read() method, it would be possible to avoid this and save a write
  ** here and there. But in practice this comes up so infrequently it is
  ** not worth the trouble.
  */
  rc = NDSFlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }

   // Let's do this the fseek way
  int currentPosition = fseek(p->fp, 0, SEEK_CUR);
  int fsize = fseek(p->fp, 0, SEEK_END);
  fseek(p->fp, currentPosition, SEEK_SET);

  *pSize = (sqlite_int64) fsize;
  //Serial.println("fn:FileSize:Success");
  return SQLITE_OK;
}

/*
** Locking functions. The xLock() and xUnlock() methods are both no-ops.
** The xCheckReservedLock() always indicates that no other process holds
** a reserved lock on the database file. This ensures that if a hot-journal
** file is found in the file-system it is rolled back.
*/
static int NDSLock(sqlite3_file *pFile, int eLock){
  return SQLITE_OK;
}
static int NDSUnlock(sqlite3_file *pFile, int eLock){
  return SQLITE_OK;
}
static int NDSCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  *pResOut = 0;
  return SQLITE_OK;
}

/*
** No xFileControl() verbs are implemented by this VFS.
*/
static int NDSFileControl(sqlite3_file *pFile, int op, void *pArg){
  return SQLITE_OK;
}

/*
** The xSectorSize() and xDeviceCharacteristics() methods. These two
** may return special values allowing SQLite to optimize file-system 
** access to some extent. But it is also safe to simply return 0.
*/
static int NDSSectorSize(sqlite3_file *pFile){
  return 0;
}
static int NDSDeviceCharacteristics(sqlite3_file *pFile){
  return 0;
}

#ifndef F_OK
# define F_OK 0
#endif
#ifndef R_OK
# define R_OK 4
#endif
#ifndef W_OK
# define W_OK 2
#endif


/*
** Query the file-system to see if the named file exists, is readable or
** is both readable and writable.
*/
static int NDSAccess(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  int rc;                         /* access() return code */
  int eAccess = F_OK;             /* Second argument to access() */
      //Serial.println("fn: Access");

  assert( flags==SQLITE_ACCESS_EXISTS       /* access(zPath, F_OK) */
       || flags==SQLITE_ACCESS_READ         /* access(zPath, R_OK) */
       || flags==SQLITE_ACCESS_READWRITE    /* access(zPath, R_OK|W_OK) */
  );

  if( flags==SQLITE_ACCESS_READWRITE ) eAccess = R_OK|W_OK;
  if( flags==SQLITE_ACCESS_READ )      eAccess = R_OK;

  rc = access(zPath, eAccess);
  *pResOut = (rc==0);
  //Serial.println("fn:Access:Success");
  return SQLITE_OK;
}

/*
** Open a file handle.
*/
static int NDSOpen(
  sqlite3_vfs *pVfs,              /* VFS */
  const char *zName,              /* File to open, or 0 for a temp file */
  sqlite3_file *pFile,            /* Pointer to ESP32File struct to populate */
  int flags,                      /* Input SQLITE_OPEN_XXX flags */
  int *pOutFlags                  /* Output SQLITE_OPEN_XXX flags (or NULL) */
){
  static const sqlite3_io_methods ESP32io = {
    1,                            /* iVersion */
    NDSClose,                    /* xClose */
    NDSRead,                     /* xRead */
    NDSWrite,                    /* xWrite */
    NDSTruncate,                 /* xTruncate */
    NDSSync,                     /* xSync */
    NDSFileSize,                 /* xFileSize */
    NDSLock,                     /* xLock */
    NDSUnlock,                   /* xUnlock */
    NDSCheckReservedLock,        /* xCheckReservedLock */
    NDSFileControl,              /* xFileControl */
    NDSSectorSize,               /* xSectorSize */
    NDSDeviceCharacteristics     /* xDeviceCharacteristics */
  };

  NDSFile *p = (NDSFile*)pFile; /* Populate this structure */
  int oflags = 0;                 /* flags to pass to open() call */
  char *aBuf = 0;
	char mode[5];
      //Serial.println("fn: Open");

	strcpy(mode, "r");
  if( zName==0 ){
    return SQLITE_IOERR;
  }

  if( flags&SQLITE_OPEN_MAIN_JOURNAL ){
    aBuf = (char *)sqlite3_malloc(SQLITE_ESP32VFS_BUFFERSZ);
    if( !aBuf ){
      return SQLITE_NOMEM;
    }
  }

	if( flags&SQLITE_OPEN_CREATE || flags&SQLITE_OPEN_READWRITE 
          || flags&SQLITE_OPEN_MAIN_JOURNAL ) {

      strcpy(mode, "w+");
    } else {
      strcpy(mode, "r+");
	}

  memset(p, 0, sizeof(NDSFile));
  //p->fd = open(zName, oflags, 0600);
  //p->fd = open(zName, oflags, S_IRUSR | S_IWUSR);
  p->fp = fopen(zName, mode);
  if(p->fp == NULL){
    if (aBuf)
      sqlite3_free(aBuf);
    //Serial.println("Can't open");
    return SQLITE_CANTOPEN;
  }
  p->aBuffer = aBuf;

  if( pOutFlags ){
    *pOutFlags = flags;
  }
  p->base.pMethods = &ESP32io;
  //Serial.println("fn:Open:Success");
  return SQLITE_OK;
}

/*
** Delete the file identified by argument zPath. If the dirSync parameter
** is non-zero, then ensure the file-system modification to delete the
** file has been synced to disk before returning.
*/
static int NDSDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  int rc;                         /* Return code */

      //Serial.println("fn: Delete");

  rc = unlink(zPath);
  if(rc!=0) return SQLITE_OK;

  if( rc==0 && dirSync ){
    FILE *dfd;                    /* File descriptor open on directory */
    int i;                        /* Iterator variable */
    char zDir[MAXPATHNAME+1];     /* Name of directory containing file zPath */

    /* Figure out the directory name from the path of the file deleted. */
    sqlite3_snprintf(MAXPATHNAME, zDir, "%s", zPath);
    zDir[MAXPATHNAME] = '\0';
    for(i=strlen(zDir); i>1 && zDir[i]!='/'; i++);
    zDir[i] = '\0';

    /* Open a file-descriptor on the directory. Sync. Close. */
    dfd = fopen(zDir, "r");
    if( dfd == NULL ){
      rc = -1;
    }else{
      rc = fflush(dfd);
      fclose(dfd);
    }
  }
  //if (rc == 0)
    //Serial.println("fn:Delete:Success");
  return (rc==0 ? SQLITE_OK : SQLITE_IOERR_DELETE);
}

/*
** Argument zPath points to a nul-terminated string containing a file path.
** If zPath is an absolute path, then it is copied as is into the output 
** buffer. Otherwise, if it is a relative path, then the equivalent full
** path is written to the output buffer.
**
** This function assumes that paths are UNIX style. Specifically, that:
**
**   1. Path components are separated by a '/'. and 
**   2. Full paths begin with a '/' character.
*/
static int NDSFullPathname(
  sqlite3_vfs *pVfs,              /* VFS */
  const char *zPath,              /* Input path (possibly a relative path) */
  int nPathOut,                   /* Size of output buffer in bytes */
  char *zPathOut                  /* Pointer to output buffer */
){
      //Serial.print("fn: FullPathName");
  //char zDir[MAXPATHNAME+1];
  //if( zPath[0]=='/' ){
  //  zDir[0] = '\0';
  //}else{
  //  if( getcwd(zDir, sizeof(zDir))==0 ) return SQLITE_IOERR;
  //}
  //zDir[MAXPATHNAME] = '\0';
	strncpy( zPathOut, zPath, nPathOut );

  //sqlite3_snprintf(nPathOut, zPathOut, "%s/%s", zDir, zPath);
  zPathOut[nPathOut-1] = '\0';
  //Serial.println("fn:Fullpathname:Success");

  return SQLITE_OK;
}

/*
** The following four VFS methods:
**
**   xDlOpen
**   xDlError
**   xDlSym
**   xDlClose
**
** are supposed to implement the functionality needed by SQLite to load
** extensions compiled as shared objects. This simple VFS does not support
** this functionality, so the following functions are no-ops.
*/
static void *NDSDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return NULL;
}
static void NDSDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
  zErrMsg[nByte-1] = '\0';
}
static void (*NDSDlSym(sqlite3_vfs *pVfs, void *pH, const char *z))(void){
  return NULL;
}
static void NDSDlClose(sqlite3_vfs *pVfs, void *pHandle){
  return;
}

/*
** Parameter zByte points to a buffer nByte bytes in size. Populate this
** buffer with pseudo-random data.
*/
static int NDSRandomness(sqlite3_vfs *pVfs, int nByte, char *zByte){
  int i;
  for(i = 0; i < nByte; i++){
	  zByte[i] = (char) rand() & 0xff;
  }
  return SQLITE_OK;
}

/*
** Sleep for at least nMicro microseconds. Return the (approximate) number 
** of microseconds slept for.
*/
#define US_CLOCK (BUS_CLOCK / (1000 * 1000))
static int NDSSleep(sqlite3_vfs *pVfs, int nMicro){
  // NDS Delay function! Implement this!
  // sleep(nMicro / 1000000);
  // usleep(nMicro % 1000000);

  cpuStartTiming(2);
  while((cpuGetTiming() / US_CLOCK) < (uint32_t) nMicro);
  cpuEndTiming();
  return nMicro;
}

/*
** Set *pTime to the current UTC time expressed as a Julian day. Return
** SQLITE_OK if successful, or an error code otherwise.
**
**   http://en.wikipedia.org/wiki/Julian_day
**
** This implementation is not very good. The current time is rounded to
** an integer number of seconds. Also, assuming time_t is a signed 32-bit 
** value, it will stop working some time in the year 2038 AD (the so-called
** "year 2038" problem that afflicts systems that store time this way). 
*/
static int NDSCurrentTime(sqlite3_vfs *pVfs, double *pTime){
  time_t t = time(0);
  *pTime = t/86400.0 + 2440587.5; 
  return SQLITE_OK;
}


sqlite3_vfs* sqlite3_ndsvfs(void){
	static sqlite3_vfs NDS_VFS = {
		1, 				// iVersion
		sizeof(NDSFile),
		MAXPATHNAME,
		0,
		"NDS",
		0,
		NDSOpen,
		NDSDelete,
		NDSAccess,
		NDSFullPathname,
		NDSDlOpen,
		NDSDlError,
		NDSDlSym,
		NDSDlClose,
		NDSRandomness,
		NDSSleep,
		NDSCurrentTime
	};

	return &NDS_VFS;
}



// SQL Init
int sqlite3_os_init(void){
	printf("sqlite3_os_init()\n");
	sqlite3_vfs_register(sqlite3_ndsvfs(), 1);
	return SQLITE_OK;
}

int sqlite3_os_end(){
	printf("sqlite_os_end()\n");
	return SQLITE_OK;
}

void OnKeyPressed(int key) {
   if(key > 0) fputc(key, stdout);
   fflush(stdout);
}

static int sql_callback(void* NotUsed, int argc, char** argv, char** azColName){
	int i;
	for(i = 0; i < argc; i++){
		printf("%s = %s\n",azColName[i], argv[i] ? argv[i] : "<NULL>");
	}
	printf("\n");
	fflush(stdout);
	return 0;
}


int main(void) {
	videoSetMode(MODE_0_2D);
	videoSetModeSub(MODE_0_2D);

	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);

	consoleInit(&topScreen, 3,BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
	consoleInit(&bottomScreen, 3,BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

	consoleSelect(&topScreen);

	Keyboard *kbd = 	keyboardDemoInit();
   	kbd->OnKeyPressed = OnKeyPressed;

	printf("NDS SQLite demo\n");
	printf("By Brais Solla G.\n");
	printf("Yes, this is absurd, i know.\n\n");

	printf("Init libfat..\n");
	if(!fatInitDefault()){
		printf("FAT init failed!\n");
		printf("Cannot continue!\nAre you in an Emulator?\n");

		while(1) swiWaitForVBlank();
	} else {
		printf("FAT init OK!\n");
	}


	printf("Init SQLite...\n");
	swiWaitForVBlank(); swiWaitForVBlank();

	sqlite3* db;
	char* sqlite_zErrMsg = NULL;
	int rc;

	rc = sqlite3_open("nds.db", &db);
	if(rc){
		printf("Database init ERROR!\n");
		while(1) swiWaitForVBlank();
	} else {
		printf("database nds.db init OK!\n");
	}
	printf("SQLite init complete!\n");

	printf("Entering REPL NOW!\n");

	while(1) {
		scanKeys();
		// REPL
		printf("> "); fflush(stdout);
		fgets(sql_query, 1023, stdin);
		// Remove newline character
		sql_query[strlen(sql_query)-1]= '\0';

		if(!strcmp("exit", sql_query)){
			// Exit
			break;
		}

		// Execute SQL Query
		rc = sqlite3_exec(db, sql_query, sql_callback, 0, &sqlite_zErrMsg);
		// Print SQL result
		if(rc != SQLITE_OK){
			printf("ERR\n");
		} else {
			printf("OK\n");
		}

	}

	sqlite3_close(db);
	printf("Exiting now!\n");
	return 0;
}
