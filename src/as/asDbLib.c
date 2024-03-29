/* share/src/as/asDbLib.c */
/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/* Author:  Marty Kraimer Date:    02-11-94*/

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "epicsStdioRedirect.h"
#include "dbDefs.h"
#include "cantProceed.h"
#include "epicsThread.h"
#include "errlog.h"
#include "taskwd.h"
#include "alarm.h"
#include "caeventmask.h"
#include "callback.h"
#include "dbStaticLib.h"
#include "dbAddr.h"
#include "dbAccess.h"
#include "db_field_log.h"
#include "dbEvent.h"
#include "dbCommon.h"
#include "recSup.h"
#define epicsExportSharedSymbols
#include "asLib.h"
#include "asCa.h"
#include "asDbLib.h"

static char	*pacf=NULL;
static char	*psubstitutions=NULL;
static epicsThreadId	asInitTheadId=0;
static int	firstTime = TRUE;

static long asDbAddRecords(void)
{
    DBENTRY	dbentry;
    DBENTRY	*pdbentry=&dbentry;
    long	status;
    dbCommon	*precord;

    dbInitEntry(pdbbase,pdbentry);
    status = dbFirstRecordType(pdbentry);
    while(!status) {
	status = dbFirstRecord(pdbentry);
	while(!status) {
	    precord = pdbentry->precnode->precord;
	    if(!precord->asp) {
		status = asAddMember(&precord->asp, precord->asg);
		if(status) errMessage(status,"asDbAddRecords:asAddMember");
		asPutMemberPvt(precord->asp,precord);
	    }
	    status = dbNextRecord(pdbentry);
	}
	status = dbNextRecordType(pdbentry);
    }
    dbFinishEntry(pdbentry);
    return(0);
}

int epicsShareAPI asSetFilename(const char *acf)
{
    if(pacf) free ((void *)pacf);
    if(acf) {
	pacf = calloc(1,strlen(acf)+1);
	if(!pacf) {
	    errMessage(0,"asSetFilename calloc failure");
	} else {
	    strcpy(pacf,acf);
	}
    } else {
	pacf = NULL;
    }
    return(0);
}

int epicsShareAPI asSetSubstitutions(const char *substitutions)
{
    if(psubstitutions) free ((void *)psubstitutions);
    if(substitutions) {
	psubstitutions = calloc(1,strlen(substitutions)+1);
	if(!psubstitutions) {
	    errMessage(0,"asSetSubstitutions calloc failure");
	} else {
	    strcpy(psubstitutions,substitutions);
	}
    } else {
	psubstitutions = NULL;
    }
    return(0);
}

static void asSpcAsCallback(struct dbCommon *precord)
{
    asChangeGroup(&precord->asp, precord->asg);
}

static void asInitCommonOnce(void *arg)
{
    int *firstTime = (int *)arg;
    *firstTime = FALSE;
}
    
static long asInitCommon(void)
{
    long	status;
    int		asWasActive = asActive;
    int		wasFirstTime = firstTime;
    static epicsThreadOnceId asInitCommonOnceFlag = EPICS_THREAD_ONCE_INIT;

    
    epicsThreadOnce(&asInitCommonOnceFlag,asInitCommonOnce,(void *)&firstTime);
    if(wasFirstTime) {
        if(!pacf) return(0); /*access security will NEVER be turned on*/
    } else {
        if(!asActive) {
            printf("Access security is NOT enabled."
                   " Was asSetFilename specified before iocInit?\n");
            return(S_asLib_asNotActive);
        }
        if(pacf) {
            asCaStop();
        } else { /*Just leave everything as is */
            return(S_asLib_badConfig);
        }
    }
    status = asInitFile(pacf,psubstitutions);
    if(asActive) {
	if(!asWasActive) {
            dbSpcAsRegisterCallback(asSpcAsCallback);
            asDbAddRecords();
        }
	asCaStart();
    }
    return(status);
}

int epicsShareAPI asInit(void)
{
    return(asInitCommon());
}

static void wdCallback(void *arg)
{
    ASDBCALLBACK *pcallback = (ASDBCALLBACK *)arg;
    pcallback->status = S_asLib_InitFailed;
    callbackRequest(&pcallback->callback);
}

static void asInitTask(ASDBCALLBACK *pcallback)
{
    long status;

    taskwdInsert(epicsThreadGetIdSelf(), wdCallback, (void *)pcallback);
    status = asInitCommon();
    taskwdRemove(epicsThreadGetIdSelf());
    asInitTheadId = 0;
    if(pcallback) {
	pcallback->status = status;
	callbackRequest(&pcallback->callback);
    }
}

int epicsShareAPI asInitAsyn(ASDBCALLBACK *pcallback)
{
    if(!pacf) return(0);
    if(asInitTheadId) {
	errMessage(-1,"asInit: asInitTask already active");
	if(pcallback) {
	    pcallback->status = S_asLib_InitFailed;
	    callbackRequest(&pcallback->callback);
	}
	return(-1);
    }
    asInitTheadId = epicsThreadCreate("asInitTask",
        (epicsThreadPriorityCAServerHigh + 1),
        epicsThreadGetStackSize(epicsThreadStackBig),
        (EPICSTHREADFUNC)asInitTask,(void *)pcallback);
    if(asInitTheadId==0) {
	errMessage(0,"asInit: epicsThreadCreate Error");
	if(pcallback) {
	    pcallback->status = S_asLib_InitFailed;
	    callbackRequest(&pcallback->callback);
	}
	asInitTheadId = 0;
    }
    return(0);
}

int epicsShareAPI asDbGetAsl(void *paddress)
{
    DBADDR	*paddr = paddress;
    dbFldDes	*pflddes;

    pflddes = paddr->pfldDes;
    return((int)pflddes->as_level);
}

void * epicsShareAPI asDbGetMemberPvt(void *paddress)
{
    DBADDR	*paddr = paddress;
    dbCommon	*precord;

    precord = paddr->precord;
    return((void *)precord->asp);
}

static void astacCallback(ASCLIENTPVT clientPvt,asClientStatus status)
{
    char *recordname;

    recordname = (char *)asGetClientPvt(clientPvt);
    printf("astac callback %s: status=%d",recordname,status);
    printf(" get %s put %s\n",(asCheckGet(clientPvt) ? "Yes" : "No"),
	(asCheckPut(clientPvt) ? "Yes" : "No"));
}

int epicsShareAPI astac(const char *pname,const char *user,const char *location)
{
    DBADDR	*paddr;
    long	status;
    ASCLIENTPVT	*pasclientpvt=NULL;
    dbCommon	*precord;
    dbFldDes	*pflddes;
    char	*puser;
    char	*plocation;

    paddr = dbCalloc(1,sizeof(DBADDR) + sizeof(ASCLIENTPVT));
    pasclientpvt = (ASCLIENTPVT *)(paddr + 1);
    status=dbNameToAddr(pname,paddr);
    if(status) {
	errMessage(status,"dbNameToAddr error");
	return(1);
    }
    precord = paddr->precord;
    pflddes = paddr->pfldDes;
    puser = asCalloc(1,strlen(user)+1);
    strcpy(puser,user);
    plocation = asCalloc(1,strlen(location)+1);
    strcpy(plocation,location);

    status = asAddClient(pasclientpvt,precord->asp,
	(int)pflddes->as_level,puser,plocation);
    if(status) {
	errMessage(status,"asAddClient error");
	return(1);
    } else {
	asPutClientPvt(*pasclientpvt,(void *)precord->name);
	asRegisterClientCallback(*pasclientpvt,astacCallback);
    }
    return(0);
}

static void myMemberCallback(ASMEMBERPVT memPvt,FILE *fp)
{
    dbCommon	*precord;

    precord = asGetMemberPvt(memPvt);
    if(precord) fprintf(fp," Record:%s",precord->name);
}

int epicsShareAPI asdbdump(void)
{
    asDumpFP(stdout,myMemberCallback,NULL,1);
    return(0);
}

int epicsShareAPI asdbdumpFP(FILE *fp)
{
    asDumpFP(fp,myMemberCallback,NULL,1);
    return(0);
}

int epicsShareAPI aspuag(const char *uagname)
{
    asDumpUagFP(stdout,uagname);
    return(0);
}

int epicsShareAPI aspuagFP(FILE *fp,const char *uagname)
{

    asDumpUagFP(fp,uagname);
    return(0);
}

int epicsShareAPI asphag(const char *hagname)
{
    asDumpHagFP(stdout,hagname);
    return(0);
}

int epicsShareAPI asphagFP(FILE *fp,const char *hagname)
{
    asDumpHagFP(fp,hagname);
    return(0);
}

int epicsShareAPI asprules(const char *asgname)
{
    asDumpRulesFP(stdout,asgname);
    return(0);
}

int epicsShareAPI asprulesFP(FILE *fp,const char *asgname)
{
    asDumpRulesFP(fp,asgname);
    return(0);
}

int epicsShareAPI aspmem(const char *asgname,int clients)
{
    asDumpMemFP(stdout,asgname,myMemberCallback,clients);
    return(0);
}

int epicsShareAPI aspmemFP(FILE *fp,const char *asgname,int clients)
{
    asDumpMemFP(fp,asgname,myMemberCallback,clients);
    return(0);
}
