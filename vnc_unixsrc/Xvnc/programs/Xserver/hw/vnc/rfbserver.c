/*
 * rfbserver.c - deal with server-side of the RFB protocol.
 */

/*
 *  Copyright (C) 2000-2006 Constantin Kaplinsky.  All Rights Reserved.
 *  Copyright (C) 2000 Tridia Corporation.  All Rights Reserved.
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

/* Use ``#define CORBA'' to enable CORBA control interface */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "windowstr.h"
#include "rfb.h"
#include "input.h"
#include "mipointer.h"
#include "sprite.h"

#ifdef CORBA
#include <vncserverctrl.h>
#endif

#define RFB_LOG(...)

char updateBuf[UPDATE_BUF_SIZE];
int ublen;

rfbClientPtr rfbClientHead = NULL;
rfbClientPtr pointerClient = NULL;  /* Mutex for pointer events */

Bool rfbAlwaysShared = FALSE;
Bool rfbNeverShared = FALSE;
Bool rfbDontDisconnect = FALSE;
Bool rfbViewOnly = FALSE; /* run server in view only mode - Ehud Karni SW */

static rfbClientPtr rfbNewClient(int sock, int udpSock);
static void rfbProcessClientProtocolVersion(rfbClientPtr cl);
static void rfbProcessClientInitMessage(rfbClientPtr cl);
static void rfbSendInteractionCaps(rfbClientPtr cl);
static void rfbProcessClientNormalMessage(rfbClientPtr cl);
static Bool rfbSendCopyRegion(rfbClientPtr cl, RegionPtr reg, int dx, int dy);
static Bool rfbSendLastRectMarker(rfbClientPtr cl);

/* Timing variables */
unsigned long serverPushInterval = 66; /* initial: 15 fps */
unsigned long retransmitTimeout = 25;
double srtt = 0.0;
double rttvar = 0.0;
/* Throughout variables */
const unsigned long tickInterval = 66;
int tickSentBytes = 0;
double sendingThroughput = 0.0;
double receivingThroughput = 100000.0;
unsigned long lastChange = 0;
/* Sequence number variables */
CARD32 seqNumCounter = 0;
CARD32 lastAckSeqNum = 0;
unsigned long lastAckTime = 0;
/* Reset compressor variables */
int handleNewBlock = 0;
CARD32 frameSeqNumCounter = 0;
CARD32 lastEventId = 0;

/* Size constants */
#define MAX_UPDATE_SIZE (2 * (1500) - 100)
#define SCREEN_XMIN (0)
#define SCREEN_XMAX (660)
#define SCREEN_YMIN (0)
#define SCREEN_YMAX (668)

typedef struct SendRegionRec {
	CARD32 seqNum;
	unsigned long time;
	int numBytes;
	RegionRec region;

	struct SendRegionRec * prev;
	struct SendRegionRec * next;
} SendRegionRec;

/* srRec (unacked queue) variables */
SendRegionRec * srRecFirst = NULL;
SendRegionRec * srRecLast = NULL;
unsigned int srRecCount = 0;

void srRecFree()
{
	SendRegionRec * cur = srRecFirst;
	SendRegionRec * next;

	while (cur != NULL) {
		next = cur->next;

		REGION_UNINIT(pScreen, &(cur->region));
		xfree(cur);

		cur = next;
	}

	srRecFirst = NULL;
	srRecLast = NULL;
	srRecCount = 0;
}

void srRecAdd(srRec)
	SendRegionRec * srRec;
{
	if (srRecLast != NULL) {
		srRecLast->next = srRec;
		srRec->prev = srRecLast;
		srRecLast = srRec;
	} else {
		srRecFirst = srRec;
		srRecLast = srRec;
	}
	srRecCount++;
}

void srRecDelete(srRec)
	SendRegionRec * srRec;
{
	SendRegionRec * prev = srRec->prev;
	SendRegionRec * next = srRec->next;

	if (prev != NULL) {
		if (next != NULL) {
			prev->next = next;
			next->prev = prev;
		} else {
			prev->next = NULL;
			srRecLast = prev;
		}
	} else {
		if (next != NULL) {
			next->prev = NULL;
			srRecFirst = next;
		} else {
			srRecFirst = NULL;
			srRecLast = NULL;
		}
	}

	REGION_UNINIT(pScreen, &(srRec->region));
	xfree(srRec);

	srRecCount--;
}

unsigned long srRecDeleteSeqNum(seqNum, numBytes)
	CARD32 seqNum;
	int * numBytes;
{
	SendRegionRec * cur = srRecFirst;
	SendRegionRec * next;

	while (cur != NULL) {
		next = cur->next;

		if (cur->seqNum == seqNum) {
			unsigned long time = cur->time;
			*numBytes = cur->numBytes;
			srRecDelete(cur);
			return time;
		}

		cur = next;
	}

	*numBytes = 0;
	return (unsigned long) 0;
}

void srRecSetupRetransmit(cl)
	rfbClientPtr cl;
{
	SendRegionRec * cur = srRecFirst;
	SendRegionRec * next;

	unsigned long now = GetTimeInMillis();

	while (cur != NULL) {
		next = cur->next;

		if (now - cur->time > retransmitTimeout) {
			REGION_UNION(pScreen, &(cl->modifiedRegion),
						 &(cl->modifiedRegion), &(cur->region));

			srRecDelete(cur);
		} else {
			break;
		}

		cur = next;
	}
}

void srRecSendRegion(regionPtr)
	RegionRec * regionPtr;
{
	SendRegionRec * cur = srRecFirst;
	SendRegionRec * next;

	while (cur != NULL) {
		next = cur->next;

		REGION_SUBTRACT(pScreen, &(cur->region), &(cur->region), regionPtr);
		if (!REGION_NOTEMPTY(pScreen, &(cur->region))) {
			srRecDelete(cur);
		}

		cur = next;
	}
}

/*
 * rfbNewClientConnection is called from sockets.c when a new connection
 * comes in.
 */

void
rfbNewClientConnection(sock, udpSock)
    int sock;
    int udpSock;
{
    rfbClientPtr cl;

    cl = rfbNewClient(sock, udpSock);

#ifdef CORBA
    if (cl != NULL)
	newConnection(cl, (KEYBOARD_DEVICE|POINTER_DEVICE), 1, 1, 1);
#endif
}


/*
 * rfbReverseConnection is called by the CORBA stuff to make an outward
 * connection to a "listening" RFB client.
 */

rfbClientPtr
rfbReverseConnection(host, port)
    char *host;
    int port;
{
    int sock;
    rfbClientPtr cl;

    if ((sock = rfbConnect(host, port)) < 0)
	return (rfbClientPtr)NULL;

    cl = rfbNewClient(sock, -1);

    if (cl) {
	cl->reverseConnection = TRUE;
    }

    return cl;
}


/*
 * rfbNewClient is called when a new connection has been made by whatever
 * means.
 */

static rfbClientPtr
rfbNewClient(sock, udpSock)
    int sock;
    int udpSock;
{
    /* srRecFree();
     * seqNumCounter = 0; // Reset sequence number for new client */

    static int clientNumber = 0;

    rfbProtocolVersionMsg pv;
    rfbClientPtr cl;
    BoxRec box;
    struct sockaddr_in addr;
    int addrlen = sizeof(struct sockaddr_in);
    int i;

    if (rfbClientHead == NULL) {
	/* no other clients - make sure we don't think any keys are pressed */
	KbdReleaseAllKeys();
    } else {
	RFB_LOG("  (other clients");
	for (cl = rfbClientHead; cl; cl = cl->next) {
	    fprintf(stderr," %s",cl->host);
	}
	fprintf(stderr,")\n");
    }

    cl = (rfbClientPtr)xalloc(sizeof(rfbClientRec));

    if (clientNumber == 0) {
    	cl->isOctopus = TRUE;
    } else {
    	cl->isOctopus = FALSE;
    }
    clientNumber++;

    cl->measuring = FALSE;
    cl->udpSock = udpSock;
    cl->useUdp = FALSE;

    cl->sock = sock;
    getpeername(sock, (struct sockaddr *)&addr, &addrlen);
    cl->host = strdup(inet_ntoa(addr.sin_addr));
    cl->login = NULL;

    /* Dispatch client input to rfbProcessClientProtocolVersion(). */
    cl->state = RFB_PROTOCOL_VERSION;

    cl->viewOnly = FALSE;
    cl->reverseConnection = FALSE;
    cl->readyForSetColourMapEntries = FALSE;
    cl->useCopyRect = FALSE;
    cl->preferredEncoding = rfbEncodingRaw;
    cl->correMaxWidth = 48;
    cl->correMaxHeight = 48;

    REGION_INIT(pScreen,&cl->copyRegion,NullBox,0);
    cl->copyDX = 0;
    cl->copyDY = 0;

    box.x1 = box.y1 = 0;
    box.x2 = rfbScreen.width;
    box.y2 = rfbScreen.height;
    REGION_INIT(pScreen,&cl->modifiedRegion,&box,0);

    REGION_INIT(pScreen,&cl->requestedRegion,NullBox,0);

    cl->deferredUpdateScheduled = FALSE;
    cl->deferredUpdateTimer = NULL;

    cl->format = rfbServerFormat;
    cl->translateFn = rfbTranslateNone;
    cl->translateLookupTable = NULL;

    cl->tightCompressLevel = TIGHT_DEFAULT_COMPRESSION;
    cl->tightQualityLevel = -1;
    for (i = 0; i < 4; i++)
        cl->zsActive[i] = FALSE;

    cl->enableCursorShapeUpdates = FALSE;
    cl->enableCursorPosUpdates = FALSE;
    cl->enableLastRectEncoding = FALSE;

    cl->next = rfbClientHead;
    rfbClientHead = cl;

    rfbResetStats(cl);

    cl->compStreamInited = FALSE;
    cl->compStream.total_in = 0;
    cl->compStream.total_out = 0;
    cl->compStream.zalloc = Z_NULL;
    cl->compStream.zfree = Z_NULL;
    cl->compStream.opaque = Z_NULL;

    cl->zlibCompressLevel = 5;

    sprintf(pv, rfbProtocolVersionFormat, 3, 8);

    if (WriteExact(sock, pv, sz_rfbProtocolVersionMsg) < 0) {
	rfbLogPerror("rfbNewClient: write");
	rfbCloseSock(sock);
	return NULL;
    }

    return cl;
}

static Bool canSend = FALSE;

/*
 * rfbClientConnectionGone is called from sockets.c just after a connection
 * has gone away.
 */

void
rfbClientConnectionGone(sock)
    int sock;
{
    rfbClientPtr cl, prev;
    int i;

    for (prev = NULL, cl = rfbClientHead; cl; prev = cl, cl = cl->next) {
	if (sock == cl->sock)
	    break;
    }

    if (!cl) {
	RFB_LOG("rfbClientConnectionGone: unknown socket %d\n",sock);
	return;
    }

    if (cl->login != NULL) {
	RFB_LOG("Client %s (%s) gone\n", cl->login, cl->host);
	free(cl->login);
    } else {
	RFB_LOG("Client %s gone\n", cl->host);
    }
    free(cl->host);

    canSend = FALSE;

    /* Release the compression state structures if any. */
    if ( cl->compStreamInited == TRUE ) {
	deflateEnd( &(cl->compStream) );
    }

    for (i = 0; i < 4; i++) {
	if (cl->zsActive[i])
	    deflateEnd(&cl->zsStruct[i]);
    }

    if (pointerClient == cl)
	pointerClient = NULL;

#ifdef CORBA
    destroyConnection(cl);
#endif

    if (prev)
	prev->next = cl->next;
    else
	rfbClientHead = cl->next;

    REGION_UNINIT(pScreen,&cl->copyRegion);
    REGION_UNINIT(pScreen,&cl->modifiedRegion);
    TimerFree(cl->deferredUpdateTimer);

    rfbPrintStats(cl);

    if (cl->translateLookupTable) free(cl->translateLookupTable);

    xfree(cl);
}

int
measureRegion(cl, x_low, y_low, x_high, y_high)
    rfbClientPtr cl;
	int x_low;
    int y_low;
    int x_high;
    int y_high;
{
    RegionRec tmpRegion;
    BoxRec box;
    int measured_size;

    box.x1 = x_low;
    box.y1 = y_low;
    box.x2 = x_high;
    box.y2 = y_high;
    SAFE_REGION_INIT(pScreen,&tmpRegion,&box,0);

    REGION_UNION(pScreen, &cl->requestedRegion, &cl->requestedRegion,
                 &tmpRegion);

    if (!cl->readyForSetColourMapEntries) {
        /* client hasn't sent a SetPixelFormat so is using server's */
        cl->readyForSetColourMapEntries = TRUE;
        if (!cl->format.trueColour) {
            if (!rfbSetClientColourMap(cl, 0, 0)) {
                REGION_UNINIT(pScreen,&tmpRegion);
                return;
            }
        }
    }

    RFB_LOG("GOING TO SEND TO CLIENT\n");
    /* RFB_LOG("  client has %d regions\n", cl->modifiedRegion.data->size); */
    RFB_LOG("MEASURING:\n");
    cl->measuring = TRUE;
    rfbSendFramebufferUpdate(cl, NULL, 0xFFFFFFFF);
    cl->measuring = FALSE;
    measured_size = ublen;
    ublen = 0;

    REGION_EMPTY(pScreen, &cl->requestedRegion);
    REGION_UNINIT(pScreen,&tmpRegion);

    return measured_size;
}

void
sendRegion(cl, x_low, y_low, x_high, y_high)
    rfbClientPtr cl;
	int x_low;
    int y_low;
    int x_high;
    int y_high;
{
    static sent_count = 0;
    sent_count++;
    RegionRec tmpRegion;
    BoxRec box;

    box.x1 = x_low;
    box.y1 = y_low;
    box.x2 = x_high;
    box.y2 = y_high;
    SAFE_REGION_INIT(pScreen,&tmpRegion,&box,0);

    REGION_UNION(pScreen, &cl->requestedRegion, &cl->requestedRegion,
                 &tmpRegion);

    if (!cl->readyForSetColourMapEntries) {
        /* client hasn't sent a SetPixelFormat so is using server's */
        cl->readyForSetColourMapEntries = TRUE;
        if (!cl->format.trueColour) {
            if (!rfbSetClientColourMap(cl, 0, 0)) {
                REGION_UNINIT(pScreen,&tmpRegion);
                return;
            }
        }
    }

    SendRegionRec * srRec = (SendRegionRec *) xalloc(sizeof(SendRegionRec));

    REGION_INIT(pScreen, &(srRec->region), NullBox, 0);
    srRec->seqNum = seqNumCounter; seqNumCounter++;

    RFB_LOG("Sending to client region %d -> %d, sent_count=%d\n", y_low, y_high, sent_count);
    cl->useUdp = TRUE;
    rfbSendFramebufferUpdate_numBytes(cl, &(srRec->region), srRec->seqNum, &(srRec->numBytes));
    cl->useUdp = FALSE;

    srRec->time = GetTimeInMillis();
    srRec->prev = NULL;
    srRec->next = NULL;
    RFB_LOG("srRec->time = %lu / srRec->seqNum = %lu, srRec->numBytes = %d\n", srRec->time, srRec->seqNum, srRec->numBytes);
    rfbLog("[P] seqNum %lu frameSeqNum %lu time %lu\n", srRec->seqNum, frameSeqNumCounter, srRec->time);

    srRecAdd(srRec);

    REGION_UNINIT(pScreen,&tmpRegion);
}

void recursiveSend(cl, x_low, y_low, x_high, y_high)
	rfbClientPtr cl;
	int x_low;
	int y_low;
	int x_high;
	int y_high;
{
    int measured_size = measureRegion(cl, x_low, y_low, x_high, y_high);
    RFB_LOG("RECURSIVE SEND (%d,%d)(%d,%d) -> %d\n", x_low, y_low, x_high, y_high, measured_size);
    if (measured_size < MAX_UPDATE_SIZE) {
        RFB_LOG("  recursiveSend: %d < %d for (%d,%d)(%d,%d)\n",
            measured_size, MAX_UPDATE_SIZE, x_low, y_low, x_high, y_high);
        tickSentBytes += measured_size;
        sendRegion(cl, x_low, y_low, x_high, y_high);
        return;
    }
    int region_count = (measured_size / MAX_UPDATE_SIZE) + 1;
    RFB_LOG("  ================\n");
    RFB_LOG("  recursiveSend: %d > %d for (%d,%d)(%d,%d) region_count=%d\n",
        measured_size, MAX_UPDATE_SIZE, x_low, y_low, x_high, y_high, region_count);

    if (region_count > 8) {
    	region_count = 8;
    }

    /* Split on longer edge. */
    int i;
    if ((x_high - x_low) > (y_high - y_low)) {
    	/* Split on x. */
    	int x_width = (x_high - x_low) / region_count;
    	for (i = 0; i < region_count; i++) {
    		recursiveSend(cl, x_low + (i*x_width),
    				y_low, x_low + ((i + 1)*x_width), y_high);
    	}
    } else {
    	/* Split on y. */
        int y_width = (y_high - y_low) / region_count;
        for (i = 0; i < region_count; i++) {
            recursiveSend(cl, x_low, y_low + (i*y_width),
            		x_high, y_low + ((i + 1)*y_width));
        }
    }
}

void
rfbServerPushClient(cl)
    rfbClientPtr cl;
{
	unsigned long now = GetTimeInMillis();
	static unsigned long last_check;
	if (now - last_check > tickInterval) {
		double t = 1000.0 * tickSentBytes / (now - last_check);

		if (sendingThroughput == 0.0) {
			sendingThroughput = t;
		} else {
			sendingThroughput = 0.75 * sendingThroughput + 0.25 * t;
		}

		last_check = now;
		tickSentBytes = 0;

		/* Linearly map quality to percentage. 1 -> 0%, 5 -> 100% */
		double qualityPercentage = (cl->tightQualityLevel - 3.0) / (3.0 - 1.0);
		/* Linearly map interval to percentage. 1000 -> 0%, 42 -> 100% */
		double intervalPercentage = (1000.0 - serverPushInterval) / (1000.0 - 42.0);

		if (sendingThroughput > receivingThroughput) {
			if (now - lastChange > 20 * tickInterval) {
				if (qualityPercentage >= intervalPercentage) {
					cl->tightQualityLevel--;
					if (cl->tightQualityLevel < 1) {
						cl->tightQualityLevel = 1;
					}
				} else {
					serverPushInterval += 5;
					if (serverPushInterval > 1000) {
						serverPushInterval = 1000;
					}
				}
				RFB_LOG("RAMP DOWN: quality = %d (%f), interval = %d (%f)\n\n", cl->tightQualityLevel, qualityPercentage, serverPushInterval, intervalPercentage);
				lastChange = now;
			}
		} else if (sendingThroughput < 0.9 * receivingThroughput) {
			if (now - lastChange > 20 * tickInterval) {
				if (qualityPercentage <= intervalPercentage) {
					cl->tightQualityLevel++;
					if (cl->tightQualityLevel > 3) {
						cl->tightQualityLevel = 3;
					}
				} else {
					serverPushInterval -= 5;
					if (serverPushInterval < 42) {
						serverPushInterval = 42;
					}
				}
				RFB_LOG("RAMP UP: quality = %d (%f), interval = %d (%f)\n\n", cl->tightQualityLevel, qualityPercentage, serverPushInterval, intervalPercentage);
				lastChange = now;
			}
		}
	}

    if (FB_UPDATE_PENDING(cl)) {

        static unsigned long last_update;
        if (now - last_update > serverPushInterval) {
        	if (canSend == FALSE) {
                return;
            }
            RFB_LOG("vvvv\n");
            RFB_LOG("rfbServerPush to client %s\n", cl->host);

            srRecSetupRetransmit(cl);

            /* Get bounding heights. */
            int x_low = cl->modifiedRegion.extents.x1;
            int y_low = cl->modifiedRegion.extents.y1;
            int x_high = cl->modifiedRegion.extents.x2;
            int y_high = cl->modifiedRegion.extents.y2;
            RFB_LOG("Bounding box is (%d,%d) -> (%d,%d)\n", x_low, y_low, x_high, y_high);

            srRecSendRegion(&(cl->modifiedRegion));

            seqNumCounter++; /* increment for new frame */
            frameSeqNumCounter++;
            recursiveSend(cl, x_low, y_low, x_high, y_high);

            last_update = now;
            RFB_LOG("^^^^\n");

			if (sendingThroughput > 0.1) {
				RFB_LOG("-> sendingThroughput = %f\n", sendingThroughput);
			}
        }
    }
}

/*
 * rfbServerPush is called to push data to all clients.
 */
void
rfbServerPush()
{
    /* Go through all clients. */
    rfbClientPtr cl;
    for (cl = rfbClientHead; cl; cl = cl->next) {
    	if (cl->isOctopus == TRUE) {
    		rfbServerPushClient(cl);
    	}
    }
}

/*
 * rfbProcessClientMessage is called when there is data to read from a client.
 */

void
rfbProcessClientMessage(sock)
    int sock;
{
    rfbClientPtr cl;

    for (cl = rfbClientHead; cl; cl = cl->next) {
	if (sock == cl->sock)
	    break;
    }

    if (!cl) {
	RFB_LOG("rfbProcessClientMessage: unknown socket %d\n",sock);
	rfbCloseSock(sock);
	return;
    }

#ifdef CORBA
    if (isClosePending(cl)) {
	RFB_LOG("Closing connection to client %s\n", cl->host);
	rfbCloseSock(sock);
	return;
    }
#endif

    switch (cl->state) {
    case RFB_PROTOCOL_VERSION:
	rfbProcessClientProtocolVersion(cl);
	break;
    case RFB_SECURITY_TYPE:	/* protocol versions 3.7 and above */
	rfbProcessClientSecurityType(cl);
	break;
    case RFB_TUNNELING_TYPE:	/* protocol versions 3.7t, 3.8t */
	rfbProcessClientTunnelingType(cl);
	break;
    case RFB_AUTH_TYPE:		/* protocol versions 3.7t, 3.8t */
	rfbProcessClientAuthType(cl);
	break;
    case RFB_AUTHENTICATION:
	rfbVncAuthProcessResponse(cl);
	break;
    case RFB_INITIALISATION:
	rfbProcessClientInitMessage(cl);
	break;
    default:
	rfbProcessClientNormalMessage(cl);
    }
}


/*
 * rfbProcessClientProtocolVersion is called when the client sends its
 * protocol version.
 */

static void
rfbProcessClientProtocolVersion(cl)
    rfbClientPtr cl;
{
    rfbProtocolVersionMsg pv;
    int n, major, minor;
    Bool mismatch;

    if ((n = ReadExact(cl->sock, pv, sz_rfbProtocolVersionMsg)) <= 0) {
	if (n == 0)
	    RFB_LOG("rfbProcessClientProtocolVersion: client gone %s\n");
	else
	    rfbLogPerror("rfbProcessClientProtocolVersion: read");
	rfbCloseSock(cl->sock);
	return;
    }

    pv[sz_rfbProtocolVersionMsg] = 0;
    if (sscanf(pv,rfbProtocolVersionFormat,&major,&minor) != 2) {
	RFB_LOG("rfbProcessClientProtocolVersion: not a valid RFB client\n");
	rfbCloseSock(cl->sock);
	return;
    }
    if (major != 3) {
	RFB_LOG("Unsupported protocol version %d.%d\n", major, minor);
	rfbCloseSock(cl->sock);
	return;
    }

    /* Always use one of the three standard versions of the RFB protocol. */
    cl->protocol_minor_ver = minor;
    if (minor > 8) {		/* buggy client */
	cl->protocol_minor_ver = 8;
    } else if (minor > 3 && minor < 7) { /* non-standard client */
	cl->protocol_minor_ver = 3;
    } else if (minor < 3) {	/* ancient client */
	cl->protocol_minor_ver = 3;
    }
    if (cl->protocol_minor_ver != minor) {
	RFB_LOG("Non-standard protocol version 3.%d, using 3.%d instead\n",
	       minor, cl->protocol_minor_ver);
    } else {
	RFB_LOG("Using protocol version 3.%d\n", cl->protocol_minor_ver);
    }

    /* TightVNC protocol extensions are not enabled yet. */
    cl->protocol_tightvnc = FALSE;

    rfbAuthNewClient(cl);
}


/*
 * rfbProcessClientInitMessage is called when the client sends its
 * initialisation message.
 */

static void
rfbProcessClientInitMessage(cl)
    rfbClientPtr cl;
{
    rfbClientInitMsg ci;
    char buf[256];
    rfbServerInitMsg *si = (rfbServerInitMsg *)buf;
    struct passwd *user;
    int len, n;
    rfbClientPtr otherCl, nextCl;

    if ((n = ReadExact(cl->sock, (char *)&ci,sz_rfbClientInitMsg)) <= 0) {
	if (n == 0)
	    RFB_LOG("rfbProcessClientInitMessage: client gone\n");
	else
	    rfbLogPerror("rfbProcessClientInitMessage: read");
	rfbCloseSock(cl->sock);
	return;
    }

    si->framebufferWidth = Swap16IfLE(rfbScreen.width);
    si->framebufferHeight = Swap16IfLE(rfbScreen.height);
    si->format = rfbServerFormat;
    si->format.redMax = Swap16IfLE(si->format.redMax);
    si->format.greenMax = Swap16IfLE(si->format.greenMax);
    si->format.blueMax = Swap16IfLE(si->format.blueMax);

    user = getpwuid(getuid());

    if (strlen(desktopName) > 128)	/* sanity check on desktop name len */
	desktopName[128] = 0;

    if (user) {
	sprintf(buf + sz_rfbServerInitMsg, "%s's %s desktop (%s:%s)",
		user->pw_name, desktopName, rfbThisHost, display);
    } else {
	sprintf(buf + sz_rfbServerInitMsg, "%s desktop (%s:%s)",
		desktopName, rfbThisHost, display);
    }
    len = strlen(buf + sz_rfbServerInitMsg);
    si->nameLength = Swap32IfLE(len);

    if (WriteExact(cl->sock, buf, sz_rfbServerInitMsg + len) < 0) {
	rfbLogPerror("rfbProcessClientInitMessage: write");
	rfbCloseSock(cl->sock);
	return;
    }

    if (cl->protocol_tightvnc)
	rfbSendInteractionCaps(cl); /* protocol 3.7t */

    /* Dispatch client input to rfbProcessClientNormalMessage(). */
    cl->state = RFB_NORMAL;

    if (!cl->reverseConnection &&
			(rfbNeverShared || (!rfbAlwaysShared && !ci.shared))) {

	if (rfbDontDisconnect) {
	    for (otherCl = rfbClientHead; otherCl; otherCl = otherCl->next) {
		if ((otherCl != cl) && (otherCl->state == RFB_NORMAL)) {
		    RFB_LOG("-dontdisconnect: Not shared & existing client\n");
		    RFB_LOG("  refusing new client %s\n", cl->host);
		    rfbCloseSock(cl->sock);
		    return;
		}
	    }
	} else {
	    for (otherCl = rfbClientHead; otherCl; otherCl = nextCl) {
		nextCl = otherCl->next;
		if ((otherCl != cl) && (otherCl->state == RFB_NORMAL)) {
		    RFB_LOG("Not shared - closing connection to client %s\n",
			   otherCl->host);
		    rfbCloseSock(otherCl->sock);
		}
	    }
	}
    }
}


/*
 * rfbSendInteractionCaps is called after sending the server
 * initialisation message, only if TightVNC protocol extensions were
 * enabled (protocol versions 3.7t, 3.8t). In this function, we send
 * the lists of supported protocol messages and encodings.
 */

/* Update these constants on changing capability lists below! */
#define N_SMSG_CAPS  0
#define N_CMSG_CAPS  0
#define N_ENC_CAPS  12

void
rfbSendInteractionCaps(cl)
    rfbClientPtr cl;
{
    rfbInteractionCapsMsg intr_caps;
    rfbCapabilityInfo enc_list[N_ENC_CAPS];
    int i;

    /* Fill in the header structure sent prior to capability lists. */
    intr_caps.nServerMessageTypes = Swap16IfLE(N_SMSG_CAPS);
    intr_caps.nClientMessageTypes = Swap16IfLE(N_CMSG_CAPS);
    intr_caps.nEncodingTypes = Swap16IfLE(N_ENC_CAPS);
    intr_caps.pad = 0;

    /* Supported server->client message types. */
    /* For future file transfer support:
    i = 0;
    SetCapInfo(&smsg_list[i++], rfbFileListData,           rfbTightVncVendor);
    SetCapInfo(&smsg_list[i++], rfbFileDownloadData,       rfbTightVncVendor);
    SetCapInfo(&smsg_list[i++], rfbFileUploadCancel,       rfbTightVncVendor);
    SetCapInfo(&smsg_list[i++], rfbFileDownloadFailed,     rfbTightVncVendor);
    if (i != N_SMSG_CAPS) {
	RFB_LOG("rfbSendInteractionCaps: assertion failed, i != N_SMSG_CAPS\n");
	rfbCloseSock(cl->sock);
	return;
    }
    */

    /* Supported client->server message types. */
    /* For future file transfer support:
    i = 0;
    SetCapInfo(&cmsg_list[i++], rfbFileListRequest,        rfbTightVncVendor);
    SetCapInfo(&cmsg_list[i++], rfbFileDownloadRequest,    rfbTightVncVendor);
    SetCapInfo(&cmsg_list[i++], rfbFileUploadRequest,      rfbTightVncVendor);
    SetCapInfo(&cmsg_list[i++], rfbFileUploadData,         rfbTightVncVendor);
    SetCapInfo(&cmsg_list[i++], rfbFileDownloadCancel,     rfbTightVncVendor);
    SetCapInfo(&cmsg_list[i++], rfbFileUploadFailed,       rfbTightVncVendor);
    if (i != N_CMSG_CAPS) {
	RFB_LOG("rfbSendInteractionCaps: assertion failed, i != N_CMSG_CAPS\n");
	rfbCloseSock(cl->sock);
	return;
    }
    */

    /* Encoding types. */
    i = 0;
    SetCapInfo(&enc_list[i++],  rfbEncodingCopyRect,       rfbStandardVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingRRE,            rfbStandardVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingCoRRE,          rfbStandardVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingHextile,        rfbStandardVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingZlib,           rfbTridiaVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingTight,          rfbTightVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingCompressLevel0, rfbTightVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingQualityLevel0,  rfbTightVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingXCursor,        rfbTightVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingRichCursor,     rfbTightVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingPointerPos,     rfbTightVncVendor);
    SetCapInfo(&enc_list[i++],  rfbEncodingLastRect,       rfbTightVncVendor);
    if (i != N_ENC_CAPS) {
	RFB_LOG("rfbSendInteractionCaps: assertion failed, i != N_ENC_CAPS\n");
	rfbCloseSock(cl->sock);
	return;
    }

    /* Send header and capability lists */
    if (WriteExact(cl->sock, (char *)&intr_caps,
		   sz_rfbInteractionCapsMsg) < 0 ||
	WriteExact(cl->sock, (char *)&enc_list[0],
		   sz_rfbCapabilityInfo * N_ENC_CAPS) < 0) {
	rfbLogPerror("rfbSendInteractionCaps: write");
	rfbCloseSock(cl->sock);
	return;
    }

    /* Dispatch client input to rfbProcessClientNormalMessage(). */
    cl->state = RFB_NORMAL;
}


/*
 * rfbProcessClientNormalMessage is called when the client has sent a normal
 * protocol message.
 */

static void
rfbProcessClientNormalMessage(cl)
    rfbClientPtr cl;
{
    int n;
    rfbClientToServerMsg msg;
    char *str;

    if ((n = ReadExact(cl->sock, (char *)&msg, 1)) <= 0) {
	if (n != 0)
	    rfbLogPerror("rfbProcessClientNormalMessage: read");
	rfbCloseSock(cl->sock);
	return;
    }

    switch (msg.type) {

    case rfbSetPixelFormat:

	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbSetPixelFormatMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}

	cl->format.bitsPerPixel = msg.spf.format.bitsPerPixel;
	cl->format.depth = msg.spf.format.depth;
	cl->format.bigEndian = (msg.spf.format.bigEndian ? 1 : 0);
	cl->format.trueColour = (msg.spf.format.trueColour ? 1 : 0);
	cl->format.redMax = Swap16IfLE(msg.spf.format.redMax);
	cl->format.greenMax = Swap16IfLE(msg.spf.format.greenMax);
	cl->format.blueMax = Swap16IfLE(msg.spf.format.blueMax);
	cl->format.redShift = msg.spf.format.redShift;
	cl->format.greenShift = msg.spf.format.greenShift;
	cl->format.blueShift = msg.spf.format.blueShift;

	cl->readyForSetColourMapEntries = TRUE;

	rfbSetTranslateFunction(cl);
	return;


    case rfbFixColourMapEntries:
	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbFixColourMapEntriesMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}
	RFB_LOG("rfbProcessClientNormalMessage: %s",
		"FixColourMapEntries unsupported\n");
	rfbCloseSock(cl->sock);
	return;


    case rfbSetEncodings:
    {
	int i;
	CARD32 enc;

	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbSetEncodingsMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}

	msg.se.nEncodings = Swap16IfLE(msg.se.nEncodings);

	cl->preferredEncoding = -1;
	cl->useCopyRect = FALSE;
	cl->enableCursorShapeUpdates = FALSE;
	cl->enableCursorPosUpdates = FALSE;
	cl->enableLastRectEncoding = FALSE;
	cl->tightCompressLevel = TIGHT_DEFAULT_COMPRESSION;
	cl->tightQualityLevel = -1;

	for (i = 0; i < msg.se.nEncodings; i++) {
	    if ((n = ReadExact(cl->sock, (char *)&enc, 4)) <= 0) {
		if (n != 0)
		    rfbLogPerror("rfbProcessClientNormalMessage: read");
		rfbCloseSock(cl->sock);
		return;
	    }
	    enc = Swap32IfLE(enc);

	    switch (enc) {

	    case rfbEncodingCopyRect:
		cl->useCopyRect = TRUE;
		break;
	    case rfbEncodingRaw:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    RFB_LOG("Using raw encoding for client %s\n",
			   cl->host);
		}
		break;
	    case rfbEncodingRRE:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    RFB_LOG("Using rre encoding for client %s\n",
			   cl->host);
		}
		break;
	    case rfbEncodingCoRRE:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    RFB_LOG("Using CoRRE encoding for client %s\n",
			   cl->host);
		}
		break;
	    case rfbEncodingHextile:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    RFB_LOG("Using hextile encoding for client %s\n",
			   cl->host);
		}
		break;
	    case rfbEncodingZlib:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    RFB_LOG("Using zlib encoding for client %s\n",
			   cl->host);
		}
              break;
	    case rfbEncodingTight:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    RFB_LOG("Using tight encoding for client %s\n",
			   cl->host);
		}
		break;
	    case rfbEncodingXCursor:
		RFB_LOG("Enabling X-style cursor updates for client %s\n",
		       cl->host);
		cl->enableCursorShapeUpdates = TRUE;
		cl->useRichCursorEncoding = FALSE;
		cl->cursorWasChanged = TRUE;
		break;
	    case rfbEncodingRichCursor:
		if (!cl->enableCursorShapeUpdates) {
		    RFB_LOG("Enabling full-color cursor updates for client "
			   "%s\n", cl->host);
		    cl->enableCursorShapeUpdates = TRUE;
		    cl->useRichCursorEncoding = TRUE;
		    cl->cursorWasChanged = TRUE;
		}
		break;
	    case rfbEncodingPointerPos:
		if (!cl->enableCursorPosUpdates) {
		    RFB_LOG("Enabling cursor position updates for client %s\n",
			   cl->host);
		    cl->enableCursorPosUpdates = TRUE;
		    cl->cursorWasMoved = TRUE;
		    cl->cursorX = -1;
		    cl->cursorY = -1;
		}
	        break;
	    case rfbEncodingLastRect:
		if (!cl->enableLastRectEncoding) {
		    RFB_LOG("Enabling LastRect protocol extension for client "
			   "%s\n", cl->host);
		    cl->enableLastRectEncoding = TRUE;
		}
		break;
	    default:
		if ( enc >= (CARD32)rfbEncodingCompressLevel0 &&
		     enc <= (CARD32)rfbEncodingCompressLevel9 ) {
		    cl->zlibCompressLevel = enc & 0x0F;
		    cl->tightCompressLevel = enc & 0x0F;
		    RFB_LOG("Using compression level %d for client %s\n",
			   cl->tightCompressLevel, cl->host);
		} else if ( enc >= (CARD32)rfbEncodingQualityLevel0 &&
			    enc <= (CARD32)rfbEncodingQualityLevel9 ) {
		    cl->tightQualityLevel = enc & 0x0F;
		    RFB_LOG("Using image quality level %d for client %s\n",
			   cl->tightQualityLevel, cl->host);
		} else {
		    RFB_LOG("rfbProcessClientNormalMessage: ignoring unknown "
			   "encoding %d\n", (int)enc);
		}
	    }
	}

	if (cl->preferredEncoding == -1) {
	    cl->preferredEncoding = rfbEncodingRaw;
	}

	if (cl->enableCursorPosUpdates && !cl->enableCursorShapeUpdates) {
	    RFB_LOG("Disabling cursor position updates for client %s\n",
		   cl->host);
	    cl->enableCursorPosUpdates = FALSE;
	}

	return;
    }


    case rfbFramebufferUpdateRequest:
    {
	RegionRec tmpRegion;
	BoxRec box;

#ifdef CORBA
	addCapability(cl, DISPLAY_DEVICE);
#endif

	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbFramebufferUpdateRequestMsg-1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}

        static int send_count = 0;
        if (cl->isOctopus) {
        	send_count++;
        	if (send_count > 10) {
				canSend = TRUE;
				/* Switch to server push mode */
				return;
			}
        	RFB_LOG("vv Sending from here, count=%d\n", send_count);
        }

	box.x1 = Swap16IfLE(msg.fur.x);
	box.y1 = Swap16IfLE(msg.fur.y);
	box.x2 = box.x1 + Swap16IfLE(msg.fur.w);
	box.y2 = box.y1 + Swap16IfLE(msg.fur.h);
	SAFE_REGION_INIT(pScreen,&tmpRegion,&box,0);

	REGION_UNION(pScreen, &cl->requestedRegion, &cl->requestedRegion,
		     &tmpRegion);

	if (!cl->readyForSetColourMapEntries) {
	    /* client hasn't sent a SetPixelFormat so is using server's */
	    cl->readyForSetColourMapEntries = TRUE;
	    if (!cl->format.trueColour) {
		if (!rfbSetClientColourMap(cl, 0, 0)) {
		    REGION_UNINIT(pScreen,&tmpRegion);
		    return;
		}
	    }
	}

	if (!msg.fur.incremental) {
	    REGION_UNION(pScreen,&cl->modifiedRegion,&cl->modifiedRegion,
			 &tmpRegion);
	    REGION_SUBTRACT(pScreen,&cl->copyRegion,&cl->copyRegion,
			    &tmpRegion);
	}

	if (FB_UPDATE_PENDING(cl)) {
	    rfbSendFramebufferUpdate(cl, NULL, 0xFFFFFFFF);
	}

	REGION_UNINIT(pScreen,&tmpRegion);
        RFB_LOG("^^ Done sending from here\n");
	return;
    }

    case rfbKeyEvent:

	cl->rfbKeyEventsRcvd++;

	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbKeyEventMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}

#ifdef CORBA
	addCapability(cl, KEYBOARD_DEVICE);

	if (!isKeyboardEnabled(cl))
	    return;
#endif

	lastEventId = msg.ke.eventId;

	if (!rfbViewOnly && !cl->viewOnly) {
	    KbdAddEvent(msg.ke.down, (KeySym)Swap32IfLE(msg.ke.key), cl);
	}
	return;


    case rfbPointerEvent:

	cl->rfbPointerEventsRcvd++;

	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbPointerEventMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}

#ifdef CORBA
	addCapability(cl, POINTER_DEVICE);

	if (!isPointerEnabled(cl))
	    return;
#endif

	if (pointerClient && (pointerClient != cl))
	    return;

	lastEventId = msg.pe.eventId;

	if (msg.pe.buttonMask == 0)
	    pointerClient = NULL;
	else
	    pointerClient = cl;

	if (!rfbViewOnly && !cl->viewOnly) {
	    cl->cursorX = (int)Swap16IfLE(msg.pe.x);
            cl->cursorY = (int)Swap16IfLE(msg.pe.y);
	    PtrAddEvent(msg.pe.buttonMask, cl->cursorX, cl->cursorY, cl);
	}
	return;


    case rfbClientCutText:

	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
			   sz_rfbClientCutTextMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseSock(cl->sock);
	    return;
	}

	msg.cct.length = Swap32IfLE(msg.cct.length);

	str = (char *)xalloc(msg.cct.length);

	if ((n = ReadExact(cl->sock, str, msg.cct.length)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    xfree(str);
	    rfbCloseSock(cl->sock);
	    return;
	}

	/* NOTE: We do not accept cut text from a view-only client */
	if (!rfbViewOnly && !cl->viewOnly)
	    rfbSetXCutText(str, msg.cct.length);

	xfree(str);
	return;

    case rfbFramebufferUpdateAck:

    	if ((n = ReadExact(cl->sock, ((char *)&msg) + 1,
    			   sz_rfbFramebufferUpdateAckMsg - 1)) <= 0) {
    	    if (n != 0)
    		rfbLogPerror("rfbProcessClientNormalMessage: read");
    	    rfbCloseSock(cl->sock);
    	    return;
    	}

    	CARD32 seqNum = Swap32IfLE(msg.fua.seqNum);
    	int numBytes;
    	unsigned long timeSent = srRecDeleteSeqNum(seqNum, &numBytes);
    	if (timeSent != 0) {
    		unsigned long timeCurrent = GetTimeInMillis();

    		double r = (double) timeCurrent - timeSent;
    		if (srtt == 0.0) {
    			srtt = r;
    			rttvar = r / 2.0;
    		} else {
    			double diff = srtt - r;
    			if (diff < 0) {
    				diff = -diff;
    			}

    			rttvar = 0.75 * rttvar + 0.25 * diff;
    			srtt = 0.875 * srtt + 0.125 * r;
    		}

    		retransmitTimeout = (unsigned long) (srtt + 2 * rttvar);
    		if (retransmitTimeout < 50) {
    			retransmitTimeout = 50;
    		}

    		if (lastAckSeqNum + 1 == seqNum) {
    			unsigned long diff = timeCurrent - lastAckTime;
    			if (diff < 1) diff = 1;
    			double t = 1000.0 * numBytes / diff;

    			if (receivingThroughput == 0.0) {
    				receivingThroughput = t;
    			} else {
    				receivingThroughput = 0.875 * receivingThroughput + 0.125 * t;
    			}

    			RFB_LOG("-> receivingThroughput = %f, numBytes = %d, time-diff = %lu\n", receivingThroughput, numBytes, diff);
    		}

    		lastAckSeqNum = seqNum;
    		lastAckTime = timeCurrent;
    	}

    	return;

    default:

	RFB_LOG("rfbProcessClientNormalMessage: unknown message type %d\n",
		msg.type);
	RFB_LOG(" ... closing connection\n");
	rfbCloseSock(cl->sock);
	return;
    }
}



/*
 * rfbSendFramebufferUpdate - send the currently pending framebuffer update to
 * the RFB client.
 */

Bool
rfbSendFramebufferUpdate(cl, theRegionPtr, seqNum)
    rfbClientPtr cl;
    RegionRec * theRegionPtr;
    CARD32 seqNum;
{
    int numBytes;
	return rfbSendFramebufferUpdate_numBytes(cl, theRegionPtr, seqNum, &numBytes);
}

Bool
rfbSendFramebufferUpdate_numBytes(cl, theRegionPtr, seqNum, numBytes)
    rfbClientPtr cl;
    RegionRec * theRegionPtr;
    CARD32 seqNum;
    int * numBytes;
{
    ScreenPtr pScreen = screenInfo.screens[0];
    int i;
    int nUpdateRegionRects;
    rfbFramebufferUpdateMsg *fu = (rfbFramebufferUpdateMsg *)updateBuf;
    RegionRec updateRegion, updateCopyRegion;
    int dx, dy;
    Bool sendCursorShape = FALSE;
    Bool sendCursorPos = FALSE;

    /*
     * If this client understands cursor shape updates, cursor should be
     * removed from the framebuffer. Otherwise, make sure it's put up.
     */

    if (cl->enableCursorShapeUpdates) {
	if (rfbScreen.cursorIsDrawn)
	    rfbSpriteRemoveCursor(pScreen);
	if (!rfbScreen.cursorIsDrawn && cl->cursorWasChanged)
	    sendCursorShape = TRUE;
    } else {
	if (!rfbScreen.cursorIsDrawn)
	    rfbSpriteRestoreCursor(pScreen);
    }

    /*
     * Do we plan to send cursor position update?
     */

    if (cl->enableCursorPosUpdates && cl->cursorWasMoved)
	sendCursorPos = TRUE;

    /*
     * The modifiedRegion may overlap the destination copyRegion.  We remove
     * any overlapping bits from the copyRegion (since they'd only be
     * overwritten anyway).
     */

    REGION_SUBTRACT(pScreen, &cl->copyRegion, &cl->copyRegion,
		    &cl->modifiedRegion);

    /*
     * The client is interested in the region requestedRegion.  The region
     * which should be updated now is the intersection of requestedRegion
     * and the union of modifiedRegion and copyRegion.  If it's empty then
     * no update is needed.
     */

    REGION_INIT(pScreen,&updateRegion,NullBox,0);
    REGION_UNION(pScreen, &updateRegion, &cl->copyRegion,
		 &cl->modifiedRegion);
    REGION_INTERSECT(pScreen, &updateRegion, &cl->requestedRegion,
		     &updateRegion);

    if (theRegionPtr != NULL) REGION_COPY(pScreen, theRegionPtr, &updateRegion);

    if ( !REGION_NOTEMPTY(pScreen,&updateRegion) &&
	 !sendCursorShape && !sendCursorPos ) {
	REGION_UNINIT(pScreen,&updateRegion);
        RFB_LOG("Region is empty, so I'm not sending\n");
	return TRUE;
    }

    /*
     * We assume that the client doesn't have any pixel data outside the
     * requestedRegion.  In other words, both the source and destination of a
     * copy must lie within requestedRegion.  So the region we can send as a
     * copy is the intersection of the copyRegion with both the requestedRegion
     * and the requestedRegion translated by the amount of the copy.  We set
     * updateCopyRegion to this.
     */

    REGION_INIT(pScreen,&updateCopyRegion,NullBox,0);
    REGION_INTERSECT(pScreen, &updateCopyRegion, &cl->copyRegion,
		     &cl->requestedRegion);
    REGION_TRANSLATE(pScreen, &cl->requestedRegion, cl->copyDX, cl->copyDY);
    REGION_INTERSECT(pScreen, &updateCopyRegion, &updateCopyRegion,
		     &cl->requestedRegion);
    dx = cl->copyDX;
    dy = cl->copyDY;

    /*
     * Next we remove updateCopyRegion from updateRegion so that updateRegion
     * is the part of this update which is sent as ordinary pixel data (i.e not
     * a copy).
     */

    REGION_SUBTRACT(pScreen, &updateRegion, &updateRegion, &updateCopyRegion);

    /*
     * Finally we leave modifiedRegion to be the remainder (if any) of parts of
     * the screen which are modified but outside the requestedRegion.  We also
     * empty both the requestedRegion and the copyRegion - note that we never
     * carry over a copyRegion for a future update.
     */

    if (!cl->measuring) {
        REGION_UNION(pScreen, &cl->modifiedRegion, &cl->modifiedRegion,
                     &cl->copyRegion);

        REGION_SUBTRACT(pScreen, &cl->modifiedRegion, &cl->modifiedRegion,
                        &updateRegion);
        REGION_SUBTRACT(pScreen, &cl->modifiedRegion, &cl->modifiedRegion,
                        &updateCopyRegion);

        REGION_EMPTY(pScreen, &cl->requestedRegion);
        REGION_EMPTY(pScreen, &cl->copyRegion);
        cl->copyDX = 0;
        cl->copyDY = 0;
    }

    /*
     * Now send the update.
     */

    if (!cl->measuring) {
        cl->rfbFramebufferUpdateMessagesSent++;
    }

    if (cl->preferredEncoding == rfbEncodingCoRRE) {
	nUpdateRegionRects = 0;

	for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
	    int x = REGION_RECTS(&updateRegion)[i].x1;
	    int y = REGION_RECTS(&updateRegion)[i].y1;
	    int w = REGION_RECTS(&updateRegion)[i].x2 - x;
	    int h = REGION_RECTS(&updateRegion)[i].y2 - y;
	    nUpdateRegionRects += (((w-1) / cl->correMaxWidth + 1)
				     * ((h-1) / cl->correMaxHeight + 1));
	}
    } else if (cl->preferredEncoding == rfbEncodingZlib) {
	nUpdateRegionRects = 0;

	for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
	    int x = REGION_RECTS(&updateRegion)[i].x1;
	    int y = REGION_RECTS(&updateRegion)[i].y1;
	    int w = REGION_RECTS(&updateRegion)[i].x2 - x;
	    int h = REGION_RECTS(&updateRegion)[i].y2 - y;
	    nUpdateRegionRects += (((h-1) / (ZLIB_MAX_SIZE( w ) / w)) + 1);
	}
    } else if (cl->preferredEncoding == rfbEncodingTight) {
	nUpdateRegionRects = 0;

	for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
	    int x = REGION_RECTS(&updateRegion)[i].x1;
	    int y = REGION_RECTS(&updateRegion)[i].y1;
	    int w = REGION_RECTS(&updateRegion)[i].x2 - x;
	    int h = REGION_RECTS(&updateRegion)[i].y2 - y;
	    int n = rfbNumCodedRectsTight(cl, x, y, w, h);
	    if (n == 0) {
		nUpdateRegionRects = 0xFFFF;
		break;
	    }
	    nUpdateRegionRects += n;
	}
    } else {
	nUpdateRegionRects = REGION_NUM_RECTS(&updateRegion);
    }

    fu->type = rfbFramebufferUpdate;

    fu->eventId = lastEventId;
    fu->seqNum = Swap32IfLE(seqNum);

    if (nUpdateRegionRects != 0xFFFF) {
	fu->nRects = Swap16IfLE(REGION_NUM_RECTS(&updateCopyRegion) +
				nUpdateRegionRects +
				!!sendCursorShape + !!sendCursorPos);
    } else {
	fu->nRects = 0xFFFF;
    }
    ublen = sz_rfbFramebufferUpdateMsg;

    if (sendCursorShape) {
        RFB_LOG("Sending cursor shape \n");
	cl->cursorWasChanged = FALSE;
	if (!rfbSendCursorShape(cl, pScreen))
	    return FALSE;
        RFB_LOG("Done sending cursor shape \n");
    }

    if (sendCursorPos) {
        RFB_LOG("Sending cursor pos \n");
	cl->cursorWasMoved = FALSE;
	if (!rfbSendCursorPos(cl, pScreen))
 	    return FALSE;
        RFB_LOG("Done sending cursor pos \n");
    }

    if (REGION_NOTEMPTY(pScreen,&updateCopyRegion)) {
	if (!rfbSendCopyRegion(cl,&updateCopyRegion,dx,dy)) {
	    REGION_UNINIT(pScreen,&updateRegion);
	    REGION_UNINIT(pScreen,&updateCopyRegion);
	    return FALSE;
	}
    }

    REGION_UNINIT(pScreen,&updateCopyRegion);

    handleNewBlock = 1;
    for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
	int x = REGION_RECTS(&updateRegion)[i].x1;
	int y = REGION_RECTS(&updateRegion)[i].y1;
	int w = REGION_RECTS(&updateRegion)[i].x2 - x;
	int h = REGION_RECTS(&updateRegion)[i].y2 - y;

	cl->rfbRawBytesEquivalent += (sz_rfbFramebufferUpdateRectHeader
				      + w * (cl->format.bitsPerPixel / 8) * h);

	switch (cl->preferredEncoding) {
	case rfbEncodingRaw:
	    if (!rfbSendRectEncodingRaw(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	case rfbEncodingRRE:
	    if (!rfbSendRectEncodingRRE(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	case rfbEncodingCoRRE:
	    if (!rfbSendRectEncodingCoRRE(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	case rfbEncodingHextile:
	    if (!rfbSendRectEncodingHextile(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	case rfbEncodingZlib:
	    if (!rfbSendRectEncodingZlib(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	case rfbEncodingTight:
	    if (!rfbSendRectEncodingTight(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	}
    }
    handleNewBlock = 0;

    REGION_UNINIT(pScreen,&updateRegion);

    if (nUpdateRegionRects == 0xFFFF && !rfbSendLastRectMarker(cl))
	return FALSE;

    RFB_LOG("Sending at end of rfbSendFramebufferUpdate\n");
    *numBytes = ublen;

    if (!rfbSendUpdateBuf(cl))
	return FALSE;

    return TRUE;
}



/*
 * Send the copy region as a string of CopyRect encoded rectangles.
 * The only slightly tricky thing is that we should send the messages in
 * the correct order so that an earlier CopyRect will not corrupt the source
 * of a later one.
 */

static Bool
rfbSendCopyRegion(cl, reg, dx, dy)
    rfbClientPtr cl;
    RegionPtr reg;
    int dx, dy;
{
    int nrects, nrectsInBand, x_inc, y_inc, thisRect, firstInNextBand;
    int x, y, w, h;
    rfbFramebufferUpdateRectHeader rect;
    rfbCopyRect cr;

    nrects = REGION_NUM_RECTS(reg);

    if (dx <= 0) {
	x_inc = 1;
    } else {
	x_inc = -1;
    }

    if (dy <= 0) {
	thisRect = 0;
	y_inc = 1;
    } else {
	thisRect = nrects - 1;
	y_inc = -1;
    }

    while (nrects > 0) {

	firstInNextBand = thisRect;
	nrectsInBand = 0;

	while ((nrects > 0) &&
	       (REGION_RECTS(reg)[firstInNextBand].y1
		== REGION_RECTS(reg)[thisRect].y1))
	{
	    firstInNextBand += y_inc;
	    nrects--;
	    nrectsInBand++;
	}

	if (x_inc != y_inc) {
	    thisRect = firstInNextBand - y_inc;
	}

	while (nrectsInBand > 0) {
	    if ((ublen + sz_rfbFramebufferUpdateRectHeader
		 + sz_rfbCopyRect) > UPDATE_BUF_SIZE)
	    {
		if (!rfbSendUpdateBuf(cl))
		    return FALSE;
	    }

	    x = REGION_RECTS(reg)[thisRect].x1;
	    y = REGION_RECTS(reg)[thisRect].y1;
	    w = REGION_RECTS(reg)[thisRect].x2 - x;
	    h = REGION_RECTS(reg)[thisRect].y2 - y;

	    rect.r.x = Swap16IfLE(x);
	    rect.r.y = Swap16IfLE(y);
	    rect.r.w = Swap16IfLE(w);
	    rect.r.h = Swap16IfLE(h);
	    rect.encoding = Swap32IfLE(rfbEncodingCopyRect);

	    memcpy(&updateBuf[ublen], (char *)&rect,
		   sz_rfbFramebufferUpdateRectHeader);
	    ublen += sz_rfbFramebufferUpdateRectHeader;

	    cr.srcX = Swap16IfLE(x - dx);
	    cr.srcY = Swap16IfLE(y - dy);

	    memcpy(&updateBuf[ublen], (char *)&cr, sz_rfbCopyRect);
	    ublen += sz_rfbCopyRect;

	    cl->rfbRectanglesSent[rfbEncodingCopyRect]++;
	    cl->rfbBytesSent[rfbEncodingCopyRect]
		+= sz_rfbFramebufferUpdateRectHeader + sz_rfbCopyRect;

	    thisRect += x_inc;
	    nrectsInBand--;
	}

	thisRect = firstInNextBand;
    }

    return TRUE;
}


/*
 * Send a given rectangle in raw encoding (rfbEncodingRaw).
 */

Bool
rfbSendRectEncodingRaw(cl, x, y, w, h)
    rfbClientPtr cl;
    int x, y, w, h;
{
    rfbFramebufferUpdateRectHeader rect;
    int nlines;
    int bytesPerLine = w * (cl->format.bitsPerPixel / 8);
    char *fbptr = (rfbScreen.pfbMemory + (rfbScreen.paddedWidthInBytes * y)
		   + (x * (rfbScreen.bitsPerPixel / 8)));

    /* Flush the buffer to guarantee correct alignment for translateFn(). */
    if (ublen > 0) {
	if (!rfbSendUpdateBuf(cl))
	    return FALSE;
    }

    rect.r.x = Swap16IfLE(x);
    rect.r.y = Swap16IfLE(y);
    rect.r.w = Swap16IfLE(w);
    rect.r.h = Swap16IfLE(h);
    rect.encoding = Swap32IfLE(rfbEncodingRaw);

    memcpy(&updateBuf[ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbRectanglesSent[rfbEncodingRaw]++;
    cl->rfbBytesSent[rfbEncodingRaw]
	+= sz_rfbFramebufferUpdateRectHeader + bytesPerLine * h;

    nlines = (UPDATE_BUF_SIZE - ublen) / bytesPerLine;

    while (TRUE) {
	if (nlines > h)
	    nlines = h;

	(*cl->translateFn)(cl->translateLookupTable, &rfbServerFormat,
			   &cl->format, fbptr, &updateBuf[ublen],
			   rfbScreen.paddedWidthInBytes, w, nlines);

	ublen += nlines * bytesPerLine;
	h -= nlines;

	if (h == 0)	/* rect fitted in buffer, do next one */
	    return TRUE;

	/* buffer full - flush partial rect and do another nlines */

	if (!rfbSendUpdateBuf(cl))
	    return FALSE;

	fbptr += (rfbScreen.paddedWidthInBytes * nlines);

	nlines = (UPDATE_BUF_SIZE - ublen) / bytesPerLine;
	if (nlines == 0) {
	    RFB_LOG("rfbSendRectEncodingRaw: send buffer too small for %d "
		   "bytes per line\n", bytesPerLine);
	    rfbCloseSock(cl->sock);
	    return FALSE;
	}
    }
}


/*
 * Send an empty rectangle with encoding field set to value of
 * rfbEncodingLastRect to notify client that this is the last
 * rectangle in framebuffer update ("LastRect" extension of RFB
 * protocol).
 */

static Bool
rfbSendLastRectMarker(cl)
    rfbClientPtr cl;
{
    rfbFramebufferUpdateRectHeader rect;

    if (ublen + sz_rfbFramebufferUpdateRectHeader > UPDATE_BUF_SIZE) {
	if (!rfbSendUpdateBuf(cl))
	    return FALSE;
    }

    rect.encoding = Swap32IfLE(rfbEncodingLastRect);
    rect.r.x = 0;
    rect.r.y = 0;
    rect.r.w = 0;
    rect.r.h = 0;

    memcpy(&updateBuf[ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbLastRectMarkersSent++;
    cl->rfbLastRectBytesSent += sz_rfbFramebufferUpdateRectHeader;

    return TRUE;
}


/*
 * Send the contents of updateBuf.  Returns 1 if successful, -1 if
 * not (errno should be set).
 */

Bool
rfbSendUpdateBuf(cl)
    rfbClientPtr cl;
{
    if (cl->measuring) {
        RFB_LOG("Measured %d bytes (not in sending mode)\n", ublen);
        return TRUE;
    }

    /*
    int i;
    for (i = 0; i < ublen; i++) {
        if (i % 10 == 0) {
            fprintf(stderr,"\n");
        }
	fprintf(stderr,"%02x ",((unsigned char *)updateBuf)[i]);
    }
    fprintf(stderr,"\n");
    */

    if (cl->useUdp) {
        if (ublen > MAX_UPDATE_SIZE) {
            RFB_LOG("Tried to send %d bytes over UDP, but too large! Killing the client. MaxUpdateSize=%d\n", ublen, MAX_UPDATE_SIZE);
            rfbCloseSock(cl->sock);
            return FALSE;
        }

        /* Create the sock_addr */
        struct sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sin_family = AF_INET;
        client_addr.sin_addr.s_addr = inet_addr(cl->host);
        client_addr.sin_port = htons(6829);

        int sent_size = sendto(cl->udpSock, updateBuf, ublen, 0,
            (struct sockaddr *)&client_addr, sizeof client_addr);

        if (sent_size == -1) {
            RFB_LOG("Error sending UDP message to %s\n", cl->host);
            rfbCloseSock(cl->sock);
            return FALSE;
        }
        if (sent_size != ublen) {
            RFB_LOG("rfbSendUpdateBuf sent %d bytes, supposed to send %d\n", sent_size, ublen);
            rfbCloseSock(cl->sock);
            return FALSE;
        }

        RFB_LOG("Sent %d bytes over UDP, ublen=%d\n", sent_size, ublen);
        ublen = 0;
        return TRUE;
    }

    RFB_LOG("rfbSendUpdateBuf is sending %d bytes\n", ublen);

    if (ublen > 0 && WriteExact(cl->sock, updateBuf, ublen) < 0) {
	rfbLogPerror("rfbSendUpdateBuf: write");
	rfbCloseSock(cl->sock);
	return FALSE;
    }

    ublen = 0;
    return TRUE;
}



/*
 * rfbSendSetColourMapEntries sends a SetColourMapEntries message to the
 * client, using values from the currently installed colormap.
 */

Bool
rfbSendSetColourMapEntries(cl, firstColour, nColours)
    rfbClientPtr cl;
    int firstColour;
    int nColours;
{
    char buf[sz_rfbSetColourMapEntriesMsg + 256 * 3 * 2];
    rfbSetColourMapEntriesMsg *scme = (rfbSetColourMapEntriesMsg *)buf;
    CARD16 *rgb = (CARD16 *)(&buf[sz_rfbSetColourMapEntriesMsg]);
    EntryPtr pent;
    int i, len;

    scme->type = rfbSetColourMapEntries;

    scme->firstColour = Swap16IfLE(firstColour);
    scme->nColours = Swap16IfLE(nColours);

    len = sz_rfbSetColourMapEntriesMsg;

    pent = (EntryPtr)&rfbInstalledColormap->red[firstColour];
    for (i = 0; i < nColours; i++) {
	if (pent->fShared) {
	    rgb[i*3] = Swap16IfLE(pent->co.shco.red->color);
	    rgb[i*3+1] = Swap16IfLE(pent->co.shco.green->color);
	    rgb[i*3+2] = Swap16IfLE(pent->co.shco.blue->color);
	} else {
	    rgb[i*3] = Swap16IfLE(pent->co.local.red);
	    rgb[i*3+1] = Swap16IfLE(pent->co.local.green);
	    rgb[i*3+2] = Swap16IfLE(pent->co.local.blue);
	}
	pent++;
    }

    len += nColours * 3 * 2;

    if (WriteExact(cl->sock, buf, len) < 0) {
	rfbLogPerror("rfbSendSetColourMapEntries: write");
	rfbCloseSock(cl->sock);
	return FALSE;
    }
    return TRUE;
}


/*
 * rfbSendBell sends a Bell message to all the clients.
 */

void
rfbSendBell()
{
    rfbClientPtr cl, nextCl;
    rfbBellMsg b;

    for (cl = rfbClientHead; cl; cl = nextCl) {
	nextCl = cl->next;
	if (cl->state != RFB_NORMAL)
	  continue;
	b.type = rfbBell;
	if (WriteExact(cl->sock, (char *)&b, sz_rfbBellMsg) < 0) {
	    rfbLogPerror("rfbSendBell: write");
	    rfbCloseSock(cl->sock);
	}
    }
}


/*
 * rfbSendServerCutText sends a ServerCutText message to all the clients.
 */

void
rfbSendServerCutText(char *str, int len)
{
    rfbClientPtr cl, nextCl;
    rfbServerCutTextMsg sct;

    if (rfbViewOnly)
	return;

    for (cl = rfbClientHead; cl; cl = nextCl) {
	nextCl = cl->next;
	if (cl->state != RFB_NORMAL || cl->viewOnly)
	  continue;
	sct.type = rfbServerCutText;
	sct.length = Swap32IfLE(len);
	if (WriteExact(cl->sock, (char *)&sct,
		       sz_rfbServerCutTextMsg) < 0) {
	    rfbLogPerror("rfbSendServerCutText: write");
	    rfbCloseSock(cl->sock);
	    continue;
	}
	if (WriteExact(cl->sock, str, len) < 0) {
	    rfbLogPerror("rfbSendServerCutText: write");
	    rfbCloseSock(cl->sock);
	}
    }
}




/*****************************************************************************
 *
 * UDP can be used for keyboard and pointer events when the underlying
 * network is highly reliable.  This is really here to support ORL's
 * videotile, whose TCP implementation doesn't like sending lots of small
 * packets (such as 100s of pen readings per second!).
 */

void
rfbNewUDPConnection(sock)
    int sock;
{
    if (write(sock, &ptrAcceleration, 1) < 0) {
	rfbLogPerror("rfbNewUDPConnection: write");
    }
}

/*
 * Because UDP is a message based service, we can't read the first byte and
 * then the rest of the packet separately like we do with TCP.  We will always
 * get a whole packet delivered in one go, so we ask read() for the maximum
 * number of bytes we can possibly get.
 */

void
rfbProcessUDPInput(sock)
    int sock;
{
    int n;
    rfbClientToServerMsg msg;

    if ((n = read(sock, (char *)&msg, sizeof(msg))) <= 0) {
	if (n < 0) {
	    rfbLogPerror("rfbProcessUDPInput: read");
	}
	rfbDisconnectUDPSock();
	return;
    }

    switch (msg.type) {

    case rfbKeyEvent:
	if (n != sz_rfbKeyEventMsg) {
	    RFB_LOG("rfbProcessUDPInput: key event incorrect length\n");
	    rfbDisconnectUDPSock();
	    return;
	}
	if (!rfbViewOnly) {
	    KbdAddEvent(msg.ke.down, (KeySym)Swap32IfLE(msg.ke.key), 0);
	}
	break;

    case rfbPointerEvent:
	if (n != sz_rfbPointerEventMsg) {
	    RFB_LOG("rfbProcessUDPInput: ptr event incorrect length\n");
	    rfbDisconnectUDPSock();
	    return;
	}
	if (!rfbViewOnly) {
	    PtrAddEvent(msg.pe.buttonMask,
			Swap16IfLE(msg.pe.x), Swap16IfLE(msg.pe.y), 0);
	}
	break;

    default:
	RFB_LOG("rfbProcessUDPInput: unknown message type %d\n",
	       msg.type);
	rfbDisconnectUDPSock();
    }
}
