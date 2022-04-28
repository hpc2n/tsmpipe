/*
    Copyright (c) 2006-2010,2012 HPC2N, Umeå University, Sweden

    Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE. 
*/

static const char rcsid[] = /*Add RCS version string to binary */
        "$Id: tsmpipe.c,v 1.8 2012/09/03 13:02:01 nikke Exp $";

/* Enable Large File Support stuff */
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE 1
#define _LARGE_FILES 1

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#include "dsmrc.h"
#include "dsmapitd.h"
#include "dsmapifp.h"

typedef enum
{
    listmode_unknown = 0,
    listmode_fsize,
    listmode_volser
} tsmpipe_listmode_t;

/* 
 * The recommended buffer size is n*TCPBUFFLEN - 4 bytes.
 * To get your buffer size, do: dsmc query options|grep TCPBUF
 * 32kB seems to be the new default, 31kB was the old.
 *
 * An additional factor is the pipe buffer size. Since we don't do threading
 * (yet), we hide a little bit of latency if we don't read more than the pipe
 * buffer can hold at a time. On Linux 2.6 this is 64kB.
 *
 * So, I would recommend a BUFLEN of 64kB if your TCPBUFLEN is above the
 * 64kB + 4 bytes limit or if your TCPBUFLEN is lower, the 
 * n*TCPBUFFLEN - 4 bytes that gets you closest to 64kB.
 *
 * For a default tuned TSM client on Linux, BUFLEN should thus be 32*1024*2-4.
 */

/* We (HPC2N) have 512kB tcpbuff */
#define BUFLEN (64*1024-4)


off_t atooff(const char *s)
{
    off_t o;

    if (sizeof(off_t) == 4)
        sscanf(s,"%d",(int *) &o);
    else if (sizeof(off_t) == 8)
        sscanf(s,"%lld",(long long *) &o);
    else {
        fprintf(stderr, "tsmpipe: atooff: Internal error\n");
        exit(1);
    }

    return o;
}


ssize_t read_full(int fd, char *buf, size_t count) {
    ssize_t done=0;

    while(count) {
        ssize_t len;

        len = read(fd, buf+done, count);
        if(len == 0) {
            break;
        }
        else if(len < 0) {
            if(errno == EINTR) {
                continue;
            }
            else {
                if(done == 0) {
                    done = -1;
                }
                break;
            }
        }
        count -= len;
        done += len;
    }

    return(done);
}


ssize_t write_full(int fd, const char *buf, size_t count)
{
    ssize_t done=0;

    while(count) {
        ssize_t len;

        len = write(fd, buf+done, count);
        if(len < 0) {
            if(errno == EINTR) {
                continue;
            }
            else {
                done = -1;
                break;
            }
        }
        count -= len;
        done += len;
    }

    return(done);
}


int tsm_checkapi(void) {
    dsmApiVersionEx     apiLibVer;
    dsUint32_t          apiVersion;
    dsUint32_t          applVersion = (10000 * DSM_API_VERSION) + 
                                      (1000 * DSM_API_RELEASE) + 
                                      (100 * DSM_API_LEVEL) + DSM_API_SUBLEVEL;

    memset(&apiLibVer, 0, sizeof(apiLibVer));
    apiLibVer.stVersion = apiVersionExVer;
    dsmQueryApiVersionEx(&apiLibVer);

    apiVersion = (10000 * apiLibVer.version) + (1000 * apiLibVer.release) + 
                 (100 * apiLibVer.level) + apiLibVer.subLevel;

    if (apiVersion < applVersion)
    { 
        printf("The Tivoli Storage Manager API library Version = %d.%d.%d.%d "
                "is at a lower version\n",
                apiLibVer.version,
                apiLibVer.release,
                apiLibVer.level,
                apiLibVer.subLevel);
        printf("than the application version = %d.%d.%d.%d.\n",
                DSM_API_VERSION,
                DSM_API_RELEASE,
                DSM_API_LEVEL,
                DSM_API_SUBLEVEL);
        printf("Please upgrade the API accordingly.\n");
        return 0;
    }

    return 1;
}


void tsm_printerr(dsUint32_t sesshandle, dsInt16_t rc, char *str) {
    char                rcStr[DSM_MAX_RC_MSG_LENGTH];

    if(rc == DSM_RC_WILL_ABORT) {
        dsUint16_t reason;

        rc = dsmEndTxn(sesshandle, DSM_VOTE_COMMIT, &reason);
        if(rc == DSM_RC_CHECK_REASON_CODE) {
            rc = reason;
        }
    }

    dsmRCMsg(sesshandle, rc, rcStr);
    fprintf(stderr, "tsmpipe: %s\n"
                    "        rc=%s\n", str, rcStr);
}


dsUint32_t tsm_initsess(char *options) {
    dsmApiVersionEx     applApi;
    dsUint32_t          sesshandle = 0;
    dsmInitExIn_t       initIn;
    dsmInitExOut_t      initOut;
    dsInt16_t           rc;

    memset(&applApi, 0, sizeof(applApi));
    applApi.version  = DSM_API_VERSION;
    applApi.release  = DSM_API_RELEASE;
    applApi.level    = DSM_API_LEVEL  ;
    applApi.subLevel = DSM_API_SUBLEVEL;

    memset(&initIn, 0, sizeof(initIn));
    initIn.stVersion        = dsmInitExInVersion;
    initIn.apiVersionExP    = &applApi;
    initIn.clientNodeNameP  = NULL;
    initIn.clientOwnerNameP = NULL;
    initIn.clientPasswordP  = NULL;
    initIn.applicationTypeP = NULL;
    initIn.configfile       = NULL;
    initIn.options          = options;
    initIn.userNameP        = NULL;
    initIn.userPasswordP    = NULL;

    memset(&initOut, 0, sizeof(initOut));

    rc = dsmInitEx(&sesshandle, &initIn, &initOut);
    if(rc == DSM_RC_REJECT_VERIFIER_EXPIRED) {
        rc = dsmChangePW(sesshandle, NULL, NULL);
        if(rc != DSM_RC_OK) {
            tsm_printerr(sesshandle, rc, "dsmChangePW failed");
            return 0;
        }
    }
    else if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmInitEx failed");
        dsmTerminate(sesshandle);
        return 0;
    }

    return sesshandle;
}


int tsm_regfs(dsUint32_t sesshandle, char *fsname) {
    regFSData       regFS;
    dsInt16_t       rc;

    memset(&regFS, 0, sizeof(regFS));

    regFS.fsName = fsname;
    regFS.fsType = "TSMPIPE";

    rc = dsmRegisterFS(sesshandle, &regFS);
    if(rc != DSM_RC_OK && rc != DSM_RC_FS_ALREADY_REGED) {
        tsm_printerr(sesshandle, rc, "dsmRegisterFS failed");
        return 0;
    }

    return 1;
}


void tsm_name2obj(char *fsname, char *filename, dsmObjName *objName) {
    char            *p;

    /* fs == "/filesystem", hl == "/directory/path", ll == "/filename" */
    strcpy(objName->fs, fsname);
    *objName->hl = '\0';
    *objName->ll = '\0';
    p = strrchr(filename, '/');
    if(p) {
        *p = '\0';
        if(*filename && *filename != '/') {
            strcpy(objName->hl, "/");
        }
        strcat(objName->hl, filename);
        sprintf(objName->ll, "/%s", p+1);
        *p = '/';
    }
    else {
        if(*filename && *filename != '/') {
            strcpy(objName->ll, "/");
        }
        strcat(objName->ll, filename);
    }
    objName->objType = DSM_OBJ_FILE;
}


int tsm_sendfile(dsUint32_t sesshandle, char *fsname, char *filename, 
                 off_t length, char *description, dsmSendType sendtype,
                 char verbose)
{
    char            *buffer;
    dsInt16_t       rc;
    dsUint16_t      reason=0;
    dsmObjName      objName;
    mcBindKey       mcBindKey;
    sndArchiveData  archData, *archDataP=NULL;
    ObjAttr         objAttr;
    DataBlk         dataBlk;
    ssize_t         nbytes;

    buffer = malloc(BUFLEN);
    if(!buffer) {
        perror("tsmpipe: malloc");
        return 0;
    }

    rc = dsmBeginTxn(sesshandle);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmBeginTxn failed");
        return 0;
    }

    tsm_name2obj(fsname, filename, &objName);

    if(verbose > 0) {
        fprintf(stderr, "tsmpipe: Starting to send stdin as %s%s%s\n",
                objName.fs, objName.hl, objName.ll);
    }

    mcBindKey.stVersion = mcBindKeyVersion;
    rc = dsmBindMC(sesshandle, &objName, sendtype, &mcBindKey);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmBindMC failed");
        return 0;
    }

    if(verbose > 1) {
        char *cgdest=NULL;

        fprintf(stderr, "tsmpipe: Bound to Management Class: %s\n", mcBindKey.mcName);
        if((sendtype == stArchiveMountWait || sendtype == stArchive) &&
                mcBindKey.archive_cg_exists)
        {
            cgdest = mcBindKey.archive_copy_dest;
        }
        else if(mcBindKey.backup_cg_exists) {
            cgdest = mcBindKey.backup_copy_dest;
        }
        if(cgdest) {
            fprintf(stderr, "tsmpipe: Destination Copy Group: %s\n", cgdest);
        }
    }

    memset(&objAttr, 0, sizeof(objAttr));
    objAttr.stVersion = ObjAttrVersion;
    *objAttr.owner = '\0';
    objAttr.sizeEstimate.hi = length >> 32;
    objAttr.sizeEstimate.lo = length & ~0U;
    objAttr.objCompressed = bFalse;

    if(sendtype == stArchiveMountWait || sendtype == stArchive) {
        archData.stVersion = sndArchiveDataVersion;
        archData.descr = description;
        archDataP = &archData;
    }

    rc = dsmSendObj(sesshandle, sendtype, archDataP, &objName, &objAttr, NULL);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmSendObj failed");
        return(0);
    }

    dataBlk.stVersion   = DataBlkVersion;

    while(1) {
        nbytes = read_full(STDIN_FILENO, buffer, BUFLEN);

        if(nbytes < 0) {
            perror("tsmpipe: read");
            return 0;
        }
        else if(nbytes == 0) {
            break;
        }

        dataBlk.bufferLen   = nbytes;
        dataBlk.numBytes    = 0;
        dataBlk.bufferPtr   = buffer;

        rc = dsmSendData(sesshandle, &dataBlk);
        if(rc != DSM_RC_OK) {
            tsm_printerr(sesshandle, rc, "dsmSendData failed");
            return 0;
        }
    }

    rc = dsmEndSendObj(sesshandle);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmEndSendObj failed");
        return(0);
    }

    rc = dsmEndTxn(sesshandle, DSM_VOTE_COMMIT, &reason);
    if(rc == DSM_RC_CHECK_REASON_CODE || 
            (rc == DSM_RC_OK && reason != DSM_RC_OK))
    {
        tsm_printerr(sesshandle, reason, "dsmEndTxn failed, reason");
        return(0);
    }
    else if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmEndTxn failed");
        return(0);
    }

    return 1;
}

/* Typedef for the callback used in tsm_queryfile() */
/* Returns: -1 upon error condition, application should exit.
 *           0 if tsm_queryfile() should skip processing the remaining
 *             matches.
 *           1 otherwise.
 */
typedef int (*tsm_query_callback)(dsmQueryType, DataBlk *, void *);


dsInt16_t tsm_queryfile(dsUint32_t sesshandle, dsmObjName *objName, 
                        char *description, dsmSendType sendtype, char verbose,
                        tsm_query_callback usercb, void * userdata)
{
    dsmQueryType        qType;
    qryArchiveData      qaData;
    qryRespArchiveData  qaResp;
    qryBackupData       qbData;
    qryRespBackupData   qbResp;
    dsmQueryBuff        *qDataP;
    DataBlk             qResp;
    dsInt16_t           rc;

    qResp.stVersion = DataBlkVersion;

    if(verbose > 1) {
        fprintf(stderr, "tsmpipe: Query file %s%s%s\n",
                objName->fs, objName->hl, objName->ll);
    }

    if(sendtype == stArchiveMountWait || sendtype == stArchive) {
        qType               = qtArchive;

        qaData.stVersion    = qryArchiveDataVersion;
        qaData.objName      = objName;
        qaData.owner        = "";
        qaData.insDateLowerBound.year = DATE_MINUS_INFINITE;
        qaData.insDateUpperBound.year = DATE_PLUS_INFINITE;
        qaData.expDateLowerBound.year = DATE_MINUS_INFINITE;
        qaData.expDateUpperBound.year = DATE_PLUS_INFINITE;
        qaData.descr        = description?description:"*";

        qDataP              = &qaData;

        qaResp.stVersion    = qryRespArchiveDataVersion;

        qResp.bufferPtr     = (char *) &qaResp;
        qResp.bufferLen     = sizeof(qaResp);
    }
    else {
        qType               = qtBackup;

        qbData.stVersion    = qryBackupDataVersion;
        qbData.objName      = objName;
        qbData.owner        = "";
        qbData.objState     = DSM_ACTIVE;
        qbData.pitDate.year = DATE_MINUS_INFINITE;

        qDataP              = &qbData;

        qbResp.stVersion    = qryRespBackupDataVersion;

        qResp.bufferPtr     = (char *) &qbResp;
        qResp.bufferLen     = sizeof(qbResp);
    }

    rc = dsmBeginQuery(sesshandle, qType, qDataP);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmBeginQuery failed");
        return rc;
    }

    /* Loop and apply our callback to the result */
    while((rc = dsmGetNextQObj(sesshandle, &qResp)) == DSM_RC_MORE_DATA) {
        int cbret;

        if(verbose > 1) {
            dsmObjName *rObjName;
            
            if(qType == qtArchive) {
                qryRespArchiveData *qr = (void *) qResp.bufferPtr;
                rObjName = &(qr->objName);
            }
            else if(qType == qtBackup) {
                qryRespBackupData *qr = (void *) qResp.bufferPtr;
                rObjName = &(qr->objName);
            }
            else {
                fprintf(stderr,
                        "tsm_queryfile: Internal error: Unknown qType %d\n",
                        qType);
                return DSM_RC_UNKNOWN_ERROR;
            }
            fprintf(stderr, "tsmpipe: Matched file %s%s%s\n",
                    rObjName->fs, rObjName->hl, rObjName->ll);
        }
        if(usercb == NULL) {
            continue;
        }
        cbret = usercb(qType, &qResp, userdata);
        if (cbret < 0) {
            return DSM_RC_UNKNOWN_ERROR;
        }
        else if(cbret == 0) {
            break;
        }
    }

    if(rc != DSM_RC_FINISHED && rc != DSM_RC_MORE_DATA) {
        tsm_printerr(sesshandle, rc, "dsmGetNextObj failed");
        return rc;
    }

    rc = dsmEndQuery(sesshandle);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmEndQuery failed");
        return rc;
    }

    return DSM_RC_OK;
}

struct matchone_cb_data {
    int             numfound;
    dsStruct64_t    objId;
    dsUint32_t      copyGroup;
};

int tsm_matchone_cb(dsmQueryType qType, DataBlk *qResp, void * userdata)
{
    struct matchone_cb_data *cbdata = userdata;

    cbdata->numfound++;

    if(qType == qtArchive) {
        qryRespArchiveData *qr = (void *) qResp->bufferPtr;
        
        cbdata->objId = qr->objId;
    }
    else if(qType == qtBackup) {
        qryRespBackupData *qr = (void *) qResp->bufferPtr;
        
        cbdata->objId       = qr->objId;
        cbdata->copyGroup   = qr->copyGroup;
    }
    else {
        fprintf(stderr,
                "tsm_matchone_cb: Internal error: Unknown qType %d\n",
                qType);
        return -1;
    }

    if(cbdata->numfound > 1) {
        fprintf(stderr, "tsmpipe: FAILED: The file specification matched multiple files.\n");
        return -1;
    }

    return 1;
}


int tsm_deletefile(dsUint32_t sesshandle, char *fsname, char *filename, 
                   char *description, dsmSendType sendtype, char verbose)
{
    dsInt16_t           rc;
    dsUint16_t          reason=0;
    dsmDelInfo          *dInfoP;
    dsmDelType          dType;
    struct matchone_cb_data  cbdata;
    delArch             daInfo;
    delBack             dbInfo;
    dsmObjName          objName;

    tsm_name2obj(fsname, filename, &objName);

    if(verbose > 0) {
        fprintf(stderr, "tsmpipe: Deleting file %s%s%s\n",
                objName.fs, objName.hl, objName.ll);
    }

    cbdata.numfound = 0;
    rc = tsm_queryfile(sesshandle, &objName, description, sendtype, 
                       verbose, tsm_matchone_cb, &cbdata);
    if(rc != DSM_RC_OK) {
        return 0;
    }

    if(cbdata.numfound == 0) {
        fprintf(stderr, "tsmpipe: FAILED: The file specification did not match any file.\n");
        return(0);
    }

    if(sendtype == stArchiveMountWait || sendtype == stArchive) {
        dType = dtArchive;

        daInfo.stVersion    = delArchVersion;
        daInfo.objId        = cbdata.objId;

        dInfoP = (dsmDelInfo *) &daInfo;
    }
    else {
        dType = dtBackup;

        dbInfo.stVersion    = delBackVersion;
        dbInfo.objNameP     = &objName;
        dbInfo.copyGroup    = cbdata.copyGroup;

        dInfoP = (dsmDelInfo *) &dbInfo;
    }

    rc = dsmBeginTxn(sesshandle);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmBeginTxn failed");
        return 0;
    }

    rc = dsmDeleteObj(sesshandle, dType, *dInfoP);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmDeleteObj failed");
        return 0;
    }

    rc = dsmEndTxn(sesshandle, DSM_VOTE_COMMIT, &reason);
    if(rc == DSM_RC_CHECK_REASON_CODE || 
            (rc == DSM_RC_OK && reason != DSM_RC_OK))
    {
        tsm_printerr(sesshandle, reason, "dsmEndTxn failed, reason");
        return(0);
    }
    else if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmEndTxn failed");
        return(0);
    }

    return(1);
}


int tsm_restorefile(dsUint32_t sesshandle, char *fsname, char *filename, 
                   char *description, dsmSendType sendtype, char verbose)
{
    dsInt16_t               rc;
    struct matchone_cb_data cbdata;
    dsmGetList              getList;
    dsmGetType              getType;
    DataBlk                 dataBlk;
    dsmObjName              objName;

    tsm_name2obj(fsname, filename, &objName);

    if(verbose > 0) {
        fprintf(stderr, "tsmpipe: Restoring file %s%s%s\n",
                objName.fs, objName.hl, objName.ll);
    }

    cbdata.numfound = 0;

    rc = tsm_queryfile(sesshandle, &objName, description, sendtype, 
                       verbose, tsm_matchone_cb, &cbdata);
    if(rc != DSM_RC_OK) {
        return 0;
    }

    if(cbdata.numfound == 0) {
        fprintf(stderr, "tsmpipe: FAILED: The file specification did not match any file.\n");
        return(0);
    }

    getList.stVersion = dsmGetListVersion;
    getList.numObjId = 1;
    getList.objId = &cbdata.objId;
    getList.partialObjData = NULL;

    if(sendtype == stArchiveMountWait || sendtype == stArchive) {
        getType = gtArchive;
    }
    else {
        getType = gtBackup;
    }

    rc = dsmBeginGetData(sesshandle, bTrue, getType, &getList);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmBeginGetData failed");
        return 0;
    }

    dataBlk.stVersion = DataBlkVersion;
    dataBlk.bufferPtr = malloc(BUFLEN);
    if(!dataBlk.bufferPtr) {
        perror("tsmpipe: malloc");
        return 0;
    }
    dataBlk.bufferLen = BUFLEN;
    dataBlk.numBytes = 0;
    rc = dsmGetObj(sesshandle, &cbdata.objId, &dataBlk);
    while(rc == DSM_RC_MORE_DATA) {
        if(write_full(STDOUT_FILENO, dataBlk.bufferPtr, dataBlk.numBytes) < 0) {
            perror("tsmpipe: write");
            return 0;
        }
        dataBlk.numBytes = 0;
        rc = dsmGetData(sesshandle, &dataBlk);
    }
    if(rc != DSM_RC_FINISHED) {
        tsm_printerr(sesshandle, rc, "dsmGetObj/dsmGetData failed");
        return 0;
    }
    if(write_full(STDOUT_FILENO, dataBlk.bufferPtr, dataBlk.numBytes) < 0) {
        perror("tsmpipe: write");
        return 0;
    }

    rc = dsmEndGetObj(sesshandle);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmEndGetObj failed");
        return 0;
    }

    rc = dsmEndGetData(sesshandle);
    if(rc != DSM_RC_OK) {
        tsm_printerr(sesshandle, rc, "dsmEndGetData failed");
        return 0;
    }

    return 1;
}

int tsm_listfile_cb(dsmQueryType qType, DataBlk *qResp, void * userdata)
{
    unsigned long long   filesize;
    dsStruct64_t        *rSizeEst;
    dsmObjName          *rObjName;
    dsUint160_t         *rOrder;
    tsmpipe_listmode_t  *listmode;

    if(userdata == NULL ) {
        fprintf(stderr, "tsm_listfile_cb: Internal error: userdata == NULL");
        return -1;
    }

    listmode = userdata;

    if(*listmode == listmode_unknown) {
        fprintf(stderr, "tsm_listfile_cb: Internal error: listmode == unknown");
        return -1;
    }

    if(qType == qtArchive) {
        qryRespArchiveData *qr = (void *) qResp->bufferPtr;
        
        rSizeEst = &qr->sizeEstimate;
        rObjName = &qr->objName;
        rOrder   = &qr->restoreOrderExt;
    }
    else if(qType == qtBackup) {
        qryRespBackupData *qr = (void *) qResp->bufferPtr;

        rSizeEst = &qr->sizeEstimate;
        rObjName = &qr->objName;
        rOrder   = &qr->restoreOrderExt;
    }
    else {
        fprintf(stderr,
                "tsm_listfile_cb: Internal error: Unknown qType %d\n", qType);
        return -1;
    }

    if(*listmode == listmode_fsize) {
        filesize = rSizeEst->hi;
        filesize <<= 32;
        filesize |= rSizeEst->lo;
        printf("%lld %s%s%s\n", 
                filesize, rObjName->fs, rObjName->hl, rObjName->ll);
    }
    else if(*listmode == listmode_volser) {
        printf("%u %s%s%s\n", 
                rOrder->top, rObjName->fs, rObjName->hl, rObjName->ll);
    }
    else {
        fprintf(stderr, "tsm_listfile_cb: Internal error: listmode %d unknown",
                *listmode);
        return -1;
    }

    return 1;
}


int tsm_listfile(dsUint32_t sesshandle, char *fsname, char *filename, 
                   char *description, dsmSendType sendtype, char verbose,
                   tsmpipe_listmode_t listmode)
{
    dsInt16_t   rc;
    dsmObjName  objName;

    tsm_name2obj(fsname, filename, &objName);

    if(verbose > 0) {
        fprintf(stderr, "tsmpipe: Listing file(s) %s%s%s\n",
                objName.fs, objName.hl, objName.ll);
    }

    rc = tsm_queryfile(sesshandle, &objName, description, sendtype, 
                       verbose, tsm_listfile_cb, &listmode);
    if(rc != DSM_RC_OK && rc != DSM_RC_ABORT_NO_MATCH) {
        return 0;
    }

    return 1;
}

int copy_env(const char *from, const char *to) {
    char *e;
    char n[PATH_MAX+1];

    e = getenv(from);
    if(!e) {
        fprintf(stderr, "tsmpipe: Environment variable %s not set\n", from);
        return(0);
    }
    n[PATH_MAX] = '\0';
    snprintf(n, PATH_MAX, "%s=%s", to, e);
    if(putenv(strdup(n))) {
        perror("tsmpipe: Setting up environment");
        return(0);
    }

    return(1);
}


void usage(void) {
    fprintf(stderr,
    "tsmpipe $Revision: 1.8 $, usage:\n"
    "tsmpipe [-A|-B] [-c|-x|-d|-t] -s fsname -f filepath [-l len]\n"
    "   -A and -B are mutually exclusive:\n"
    "       -A  Use Archive objects\n"
    "       -B  Use Backup objects\n"
    "   -c, -x, -d and -t are mutually exclusive:\n"
    "       -c  Create:  Read from stdin and store in TSM\n"
    "       -x  eXtract: Recall from TSM and write to stdout\n"
    "       -d  Delete:  Delete object from TSM\n"
    "       -t  lisT:    Print filelist with filesizes to stdout\n"
    "       -T  lisT:    Print filelist with volser ids to stdout\n"
    "   -s and -f are required arguments:\n"
    "       -s fsname   Name of filesystem in TSM\n"
    "       -f filepath Path to file within filesystem in TSM\n"
    "   -l length   Length of object to store. If guesstimating too large\n"
    "               is better than too small\n"
    "   -D desc     Description of archive object\n"
    "   -O options  Extra options to pass to dsmInitEx\n"
    "   -v          Verbose. More -v's gives more verbosity\n"
    );
}


int main(int argc, char *argv[]) {
    int         c;
    extern int  optind, optopt;
    extern char *optarg;
    char        archmode=0, backmode=0, create=0, xtract=0, delete=0, verbose=0;
    char        list=0;
    char        *space=NULL, *filename=NULL, *lenstr=NULL, *desc=NULL;
    char        *options=NULL;
    off_t       length;
    dsUint32_t  sesshandle;
    dsmSendType sendtype;
    tsmpipe_listmode_t listmode=listmode_unknown;

    while ((c = getopt(argc, argv, "hABcxdtTvs:f:l:D:O:")) != -1) {
        switch(c) {
            case 'h':
                usage();
                exit(1);
            case 'A':
                archmode = 1;
                break;
            case 'B':
                backmode = 1;
                break;
            case 'c':
                create = 1;
                break;
            case 'x':
                xtract = 1;
                break;
            case 'd':
                delete = 1;
                break;
            case 't':
                list = 1;
                listmode = listmode_fsize;
                break;
            case 'T':
                list = 1;
                listmode = listmode_volser;
                break;
            case 'v':
                verbose++;
                break;
            case 's':
                space = optarg;
                break;
            case 'f':
                filename = optarg;
                break;
            case 'l':
                lenstr = optarg;
                break;
            case 'D':
                desc = optarg;
                break;
            case 'O':
                options = optarg;
                break;
            case ':':
                fprintf(stderr, "tsmpipe: Option -%c requires an operand\n", optopt);
                exit(1);
            case '?':
                fprintf(stderr, "tsmpipe: Unrecognized option: -%c\n", optopt);
                exit(1);
        }
    }

    if(archmode+backmode != 1) {
        fprintf(stderr, "tsmpipe: ERROR: Must give one of -A or -B\n");
        exit(1);
    }
    if(create+xtract+delete+list != 1) {
        fprintf(stderr, "tsmpipe: ERROR: Must give one of -c, -x, -d or -t\n");
        exit(1);
    }
    if(!space) {
        fprintf(stderr, "tsmpipe: ERROR: Must give -s filespacename\n");
        exit(1);
    }
    if(!filename) {
        fprintf(stderr, "tsmpipe: ERROR: Must give -f filename\n");
        exit(1);
    }
    if(create && !lenstr) {
        fprintf(stderr, "tsmpipe: ERROR: Must give -l length with -c\n");
        exit(1);
    }
    if(!create && lenstr) {
        fprintf(stderr, "tsmpipe: ERROR: -l length useless without -c\n");
        exit(1);
    }
    if(!archmode && desc) {
        fprintf(stderr, "tsmpipe: ERROR: -D desc useless without -A\n");
        exit(1);
    }

    if(archmode) {
        sendtype = stArchiveMountWait;
    }
    else {
        sendtype = stBackupMountWait;
    }

    /* Let the TSM api get the signals */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);

    if(!copy_env("DSM_DIR", "DSMI_DIR")) {
        exit(2);
    }

    if(!copy_env("DSM_CONFIG", "DSMI_CONFIG")) {
        exit(2);
    }

    if(!tsm_checkapi()) {
        exit(2);
    }

    sesshandle = tsm_initsess(options);
    if(!sesshandle) {
        exit(3);
    }

    if(verbose > 1) {
        fprintf(stderr, "tsmpipe: Session initiated\n");
    }

    if(create) {
        if(!tsm_regfs(sesshandle, space)) {
            exit(4);
        }
        length = atooff(lenstr);
        if(length <= 0) {
            fprintf(stderr, "tsmpipe: ERROR: Provide positive length, overestimate if guessing");
            exit(5);
        }
        if(!tsm_sendfile(sesshandle, space, filename, length, desc, sendtype, verbose)){
            dsmTerminate(sesshandle);
            exit(6);
        }
    }

    if(delete) {
        if(!tsm_deletefile(sesshandle, space, filename, desc, sendtype, verbose)) {
            dsmTerminate(sesshandle);
            exit(7);
        }
    }

    if(xtract) {
        if(!tsm_restorefile(sesshandle, space, filename, desc, sendtype, verbose))
        {
            dsmTerminate(sesshandle);
            exit(8);
        }
    }

    if(list) {
        if(!tsm_listfile(sesshandle, space, filename, desc, sendtype, verbose, listmode))
        {
            dsmTerminate(sesshandle);
            exit(9);
        }
    }

    if(verbose > 0) {
        fprintf(stderr, "tsmpipe: Success!\n");
    }

    dsmTerminate(sesshandle);

    return(0);
}


/*
vim:ts=4:sw=4:et:cindent
*/
