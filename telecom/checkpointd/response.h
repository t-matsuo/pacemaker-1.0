/* $Id: response.h,v 1.2 2004/02/17 22:12:02 lars Exp $ */
#ifndef _CKPT_RESPONSE_H
#define _CKPT_RESPONSE_H

#include <saf/ais.h>
#include <checkpointd/clientrequest.h>
#include "request.h"

typedef struct _SaCkptClientResponseT {
	SaCkptHandleT	clientHandle;
	SaUint32T	requestNO;

	SaErrorT	retVal;

	SaUint32T	dataLength;
	void*		data;
} SaCkptClientResponseT;

typedef struct _SaCkptResponseT {
	SaCkptRequestT*	ckptReq;
	SaCkptClientResponseT*	resp;
} SaCkptResponseT;	

SaCkptResponseT* SaCkptResponseCreate(SaCkptRequestT*);
int SaCkptResponseSend(SaCkptResponseT**);


#endif
