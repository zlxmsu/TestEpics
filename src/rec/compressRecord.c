/*************************************************************************\
* Copyright (c) 2008 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/

/* Revision-Id: anj@aps.anl.gov-20110608161626-57vf3rfrk73ppemb */
/*
 *      Original Author: Bob Dalesio
 *      Date:            7-14-89 
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "dbDefs.h"
#include "epicsPrint.h"
#include "alarm.h"
#include "dbStaticLib.h"
#include "dbAccess.h"
#include "dbEvent.h"
#include "dbFldTypes.h"
#include "errMdef.h"
#include "special.h"
#include "recSup.h"
#include "recGbl.h"
#define GEN_SIZE_OFFSET
#include "compressRecord.h"
#undef  GEN_SIZE_OFFSET
#include "epicsExport.h"

/* Create RSET - Record Support Entry Table*/
#define report NULL
#define initialize NULL
static long init_record(compressRecord *, int);
static long process(compressRecord *);
static long special(DBADDR *, int);
#define get_value NULL
static long cvt_dbaddr(DBADDR *);
static long get_array_info(DBADDR *, long *, long *);
static long put_array_info(DBADDR *, long);
static long get_units(DBADDR *, char *);
static long get_precision(DBADDR *, long *);
#define get_enum_str NULL
#define get_enum_strs NULL
#define put_enum_str NULL
static long get_graphic_double(DBADDR *, struct dbr_grDouble *);
static long get_control_double(DBADDR *, struct dbr_ctrlDouble *);
#define get_alarm_double NULL

rset compressRSET={
	RSETNUMBER,
	report,
	initialize,
	init_record,
	process,
	special,
	get_value,
	cvt_dbaddr,
	get_array_info,
	put_array_info,
	get_units,
	get_precision,
	get_enum_str,
	get_enum_strs,
	put_enum_str,
	get_graphic_double,
	get_control_double,
	get_alarm_double
};
epicsExportAddress(rset,compressRSET);

static void reset(compressRecord *prec)
{
    prec->nuse = 0;
    prec->off = 0;
    prec->inx = 0;
    prec->cvb = 0.0;
    prec->res = 0;
    /* allocate memory for the summing buffer for conversions requiring it */
    if (prec->alg == compressALG_Average && prec->sptr == 0){
        prec->sptr = (double *)calloc(prec->nsam,sizeof(double));
    }
}

static void monitor(compressRecord *prec)
{
    unsigned short alarm_mask = recGblResetAlarms(prec);
    unsigned short monitor_mask = alarm_mask | DBE_LOG | DBE_VALUE;

    if (alarm_mask || prec->nuse != prec->ouse) {
        db_post_events(prec, &prec->nuse, monitor_mask);
        prec->ouse = prec->nuse;
    }
    db_post_events(prec, prec->bptr, monitor_mask);
}

static void put_value(compressRecord *prec,double *psource, epicsInt32 n)
{
/* treat bptr as pointer to a circular buffer*/
	double *pdest;
	epicsInt32 offset=prec->off;
	epicsInt32 nuse=prec->nuse;
	epicsInt32 nsam=prec->nsam;
	epicsInt32 i;

	pdest = prec->bptr + offset;
	for(i=0; i<n; i++, psource++) {
		*pdest=*psource;
		offset++;
		if(offset>=nsam) {
			pdest=prec->bptr;
			offset=0;
		} else pdest++;
	}
	nuse = nuse+n;
	if(nuse>nsam) nuse=nsam;
	prec->off = offset;
	prec->nuse = nuse;
	return;
}

/* qsort comparison function (for median calculation) */
static int compare(const void *arg1, const void *arg2)
{
    double a = *(double *)arg1;
    double b = *(double *)arg2;

    if      ( a <  b ) return -1;
    else if ( a == b ) return  0;
    else               return  1;
}

static int compress_array(compressRecord *prec,
	double *psource,epicsInt32 no_elements)
{
	epicsInt32	i,j;
	epicsInt32	nnew;
	epicsInt32	nsam=prec->nsam;
	double		value;
	epicsInt32	n;

	/* skip out of limit data */
	if (prec->ilil < prec->ihil){
	    while (((*psource < prec->ilil) || (*psource > prec->ihil))
		 && (no_elements > 0)){
		    no_elements--;
		    psource++;
	    }
	}
	if(prec->n <= 0) prec->n = 1;
	n = prec->n;
	if(no_elements<n) return(1); /*dont do anything*/

	/* determine number of samples to take */
	if (no_elements < (nsam * n)) nnew = (no_elements / n);
	else nnew = nsam;

	/* compress according to specified algorithm */
	switch (prec->alg){
	case (compressALG_N_to_1_Low_Value):
	    /* compress N to 1 keeping the lowest value */
	    for (i = 0; i < nnew; i++){
		value = *psource++;
		for (j = 1; j < n; j++, psource++){
		    if (value > *psource) value = *psource;
		}
		put_value(prec,&value,1);
	    }
	    break;
	case (compressALG_N_to_1_High_Value):
	    /* compress N to 1 keeping the highest value */
	    for (i = 0; i < nnew; i++){
		value = *psource++;
		for (j = 1; j < n; j++, psource++){
		    if (value < *psource) value = *psource;
		}
		put_value(prec,&value,1);
	    }
	    break;
	case (compressALG_N_to_1_Average):
	    /* compress N to 1 keeping the average value */
	    for (i = 0; i < nnew; i++){
		value = 0;
		for (j = 0; j < n; j++, psource++)
			value += *psource;
		value /= n;
		put_value(prec,&value,1);
	    }
	    break;
        case (compressALG_N_to_1_Median):
            /* compress N to 1 keeping the median value */
	    /* note: sorts source array (OK; it's a work pointer) */
            for (i = 0; i < nnew; i++, psource+=nnew){
	        qsort(psource,n,sizeof(double),compare);
		value=psource[n/2];
                put_value(prec,&value,1);
            }
            break;
        }
	return(0);
}

static int array_average(compressRecord *prec,
	double *psource,epicsInt32 no_elements)
{
	epicsInt32	i;
	epicsInt32	nnow;
	epicsInt32	nsam=prec->nsam;
	double	*psum;
	double	multiplier;
	epicsInt32	inx=prec->inx;
	epicsInt32	nuse,n;

	nuse = nsam;
	if(nuse>no_elements) nuse = no_elements;
	nnow=nuse;
	if(nnow>no_elements) nnow=no_elements;
	psum = (double *)prec->sptr;

	/* add in the new waveform */
	if (inx == 0){
		for (i = 0; i < nnow; i++)
		    *psum++ = *psource++;
		for(i=nnow; i<nuse; i++) *psum++ = 0;
	}else{
		for (i = 0; i < nnow; i++)
		    *psum++ += *psource++;
	}

	/* do we need to calculate the result */
	inx++;
	if(prec->n<=0)prec->n=1;
	n = prec->n;
	if (inx<n) {
		prec->inx = inx;
		return(1);
	}
	if(n>1) {
		psum = (double *)prec->sptr;
		multiplier = 1.0/((double)n);
		for (i = 0; i < nuse; i++, psum++)
	    		*psum = *psum * multiplier;
	}
	put_value(prec,prec->sptr,nuse);
	prec->inx = 0;
	return(0);
}

static int compress_scalar(struct compressRecord *prec,double *psource)
{
	double	value = *psource;
	double	*pdest=&prec->cvb;
	epicsInt32	inx = prec->inx;

	/* compress according to specified algorithm */
	switch (prec->alg){
	case (compressALG_N_to_1_Low_Value):
	    if ((value < *pdest) || (inx == 0))
		*pdest = value;
	    break;
	case (compressALG_N_to_1_High_Value):
	    if ((value > *pdest) || (inx == 0))
		*pdest = value;
	    break;
	/* for scalars, Median not implemented => use average */
	case (compressALG_N_to_1_Average):
	case (compressALG_N_to_1_Median):
	    if (inx == 0)
		*pdest = value;
	    else {
		*pdest += value;
		if(inx+1>=(prec->n)) *pdest = *pdest/(inx+1);
	    }
	    break;
	}
	inx++;
	if(inx>=prec->n) {
		put_value(prec,pdest,1);
		prec->inx = 0;
		return(0);
	} else {
		prec->inx = inx;
		return(1);
	}
}

/*Beginning of record support routines*/
static long init_record(compressRecord *prec, int pass)
{
    if (pass==0){
	if(prec->nsam<1) prec->nsam = 1;
	prec->bptr = (double *)calloc(prec->nsam,sizeof(double));
        reset(prec);
    }
    return(0);
}

static long process(compressRecord *prec)
{
    long	status=0;
    long	nelements = 0;
    int		alg = prec->alg;

    prec->pact = TRUE;
    if(!dbIsLinkConnected(&prec->inp)
    || dbGetNelements(&prec->inp,&nelements)
    || nelements<=0) {
	recGblSetSevr(prec,LINK_ALARM,INVALID_ALARM);
    } else {
	if(!prec->wptr || nelements!=prec->inpn) {
            if(prec->wptr) {
                free(prec->wptr);
                reset(prec);
            }
	    prec->wptr = (double *)dbCalloc(nelements,sizeof(double));
	    prec->inpn = nelements;
	}
	status = dbGetLink(&prec->inp,DBF_DOUBLE,prec->wptr,0,&nelements);
	if(status || nelements<=0) {
            recGblSetSevr(prec,LINK_ALARM,INVALID_ALARM);
	    status = 0;
	} else {
	    if(alg==compressALG_Average) {
		status = array_average(prec,prec->wptr,nelements);
	    } else if(alg==compressALG_Circular_Buffer) {
		(void)put_value(prec,prec->wptr,nelements);
		status = 0;
	    } else if(nelements>1) {
		status = compress_array(prec,prec->wptr,nelements);
	    }else if(nelements==1){
		status = compress_scalar(prec,prec->wptr);
	    }else status=1;
	}
    }
    /* check event list */
    if(status!=1) {
		prec->udf=FALSE;
		recGblGetTimeStamp(prec);
		monitor(prec);
		/* process the forward scan link record */
		recGblFwdLink(prec);
    }
    prec->pact=FALSE;
    return(0);
}

static long special(DBADDR *paddr, int after)
{
    compressRecord   *prec = (compressRecord *)(paddr->precord);
    int                 special_type = paddr->special;

    if(!after) return(0);
    switch(special_type) {
    case(SPC_RESET):
	reset(prec);
        return(0);
    default:
        recGblDbaddrError(S_db_badChoice,paddr,"compress: special");
        return(S_db_badChoice);
    }
}

static long cvt_dbaddr(DBADDR *paddr)
{
    compressRecord *prec=(compressRecord *)paddr->precord;

    paddr->pfield = (void *)(prec->bptr);
    paddr->no_elements = prec->nsam;
    paddr->field_type = DBF_DOUBLE;
    paddr->field_size = sizeof(double);
    paddr->dbr_field_type = DBF_DOUBLE;
    return(0);
}

static long get_array_info(DBADDR *paddr,long *no_elements, long *offset)
{
    compressRecord *prec=(compressRecord *)paddr->precord;

    *no_elements =  prec->nuse;
    if(prec->nuse==prec->nsam) *offset = prec->off;
    else *offset = 0;
    return(0);
}

static long put_array_info(DBADDR *paddr, long nNew)
{
    compressRecord *prec=(compressRecord *)paddr->precord;

    prec->off = (prec->off + nNew) % (prec->nsam);
    prec->nuse = (prec->nuse + nNew);
    if(prec->nuse > prec->nsam) prec->nuse = prec->nsam;
    return(0);
}

static long get_units(DBADDR *paddr,char *units)
{
    compressRecord *prec=(compressRecord *)paddr->precord;

    strncpy(units,prec->egu,DB_UNITS_SIZE);
    return(0);
}

static long get_precision(DBADDR *paddr, long *precision)
{
    compressRecord	*prec=(compressRecord *)paddr->precord;

    *precision = prec->prec;
    if(paddr->pfield == (void *)prec->bptr) return(0);
    recGblGetPrec(paddr,precision);
    return(0);
}

static long get_graphic_double(DBADDR *paddr,struct dbr_grDouble *pgd)
{
    compressRecord *prec=(compressRecord *)paddr->precord;

    if(paddr->pfield==(void *)prec->bptr
    || paddr->pfield==(void *)&prec->ihil
    || paddr->pfield==(void *)&prec->ilil){
        pgd->upper_disp_limit = prec->hopr;
        pgd->lower_disp_limit = prec->lopr;
    } else recGblGetGraphicDouble(paddr,pgd);
    return(0);
}

static long get_control_double(DBADDR *paddr, struct dbr_ctrlDouble *pcd)
{
    compressRecord *prec=(compressRecord *)paddr->precord;

    if(paddr->pfield==(void *)prec->bptr
    || paddr->pfield==(void *)&prec->ihil
    || paddr->pfield==(void *)&prec->ilil){
        pcd->upper_ctrl_limit = prec->hopr;
        pcd->lower_ctrl_limit = prec->lopr;
    } else recGblGetControlDouble(paddr,pcd);
    return(0);
}
