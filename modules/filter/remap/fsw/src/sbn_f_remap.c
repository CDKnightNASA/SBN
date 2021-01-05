#include "sbn_interfaces.h"
#include "sbn_remap_tbl.h"
#include "sbn_msgids.h"
#include "cfe.h"
#include "cfe_tbl.h"
#include <string.h> /* memcpy */
#include <stdlib.h> /* qsort */

#include "sbn_f_remap_events.h"

OS_MutexID_t    RemapMutex  = 0;
SBN_RemapTbl_t *RemapTbl    = NULL;
int             RemapTblCnt = 0;

CFE_EVS_EventID_t SBN_F_REMAP_FIRST_EID;

static int RemapTblVal(void *TblPtr)
{
    SBN_RemapTbl_t *r = (SBN_RemapTbl_t *)TblPtr;
    int             i = 0;

    switch (r->RemapDefaultFlag)
    {
        /* all valid values */
        case SBN_REMAP_DEFAULT_IGNORE:
        case SBN_REMAP_DEFAULT_SEND:
            break;
        /* otherwise, unknown! */
        default:
            return -1;
    } /* end switch */

    /* Find the first "empty" entry (with a 0x0000 "from") to determine table
     * size.
     */
    for (i = 0; i < SBN_REMAP_TABLE_SIZE; i++)
    {
        if (r->Entries[i].FromMID == 0x0000)
        {
            break;
        } /* end if */
    }     /* end for */

    RemapTblCnt = i;

    return 0;
} /* end RemapTblVal() */

static int RemapTblCompar(const void *a, const void *b)
{
    SBN_RemapTblEntry_t *aEntry = (SBN_RemapTblEntry_t *)a;
    SBN_RemapTblEntry_t *bEntry = (SBN_RemapTblEntry_t *)b;

    if (aEntry->ProcessorID != bEntry->ProcessorID)
    {
        return aEntry->ProcessorID - bEntry->ProcessorID;
    }
    return aEntry->FromMID - bEntry->FromMID;
} /* end RemapTblCompar() */

static SBN_Status_t LoadRemapTbl(void)
{
    CFE_TBL_Handle_t RemapTblHandle = 0;
    SBN_RemapTbl_t * TblPtr         = NULL;
    CFE_Status_t     CFE_Status     = CFE_SUCCESS;

    if (CFE_TBL_Register(&RemapTblHandle, "SBN_RemapTbl", sizeof(SBN_RemapTbl_t), CFE_TBL_OPT_DEFAULT, &RemapTblVal) !=
        CFE_SUCCESS)
    {
        EVSSendErr(SBN_F_REMAP_TBL_EID, "unable to register remap tbl handle");
        return SBN_ERROR;
    } /* end if */

    if (CFE_TBL_Load(RemapTblHandle, CFE_TBL_SRC_FILE, SBN_REMAP_TBL_FILENAME) != CFE_SUCCESS)
    {
        EVSSendErr(SBN_F_REMAP_TBL_EID, "unable to load remap tbl %s", SBN_REMAP_TBL_FILENAME);
        CFE_TBL_Unregister(RemapTblHandle);
        return SBN_ERROR;
    } /* end if */

    if ((CFE_Status = CFE_TBL_GetAddress((void **)&TblPtr, RemapTblHandle)) != CFE_TBL_INFO_UPDATED)
    {
        EVSSendErr(SBN_F_REMAP_TBL_EID, "unable to get conf table address");
        CFE_TBL_Unregister(RemapTblHandle);
        return SBN_ERROR;
    } /* end if */

    /* sort the entries on <ProcessorID> and <from MID> */
    /* note: qsort is recursive, so it will use some stack space
     * (O[N log N] * <some small amount of stack>). If this is a concern,
     * consider using a non-recursive (insertion, bubble, etc) sort algorithm.
     */

    qsort(TblPtr->Entries, RemapTblCnt, sizeof(SBN_RemapTblEntry_t), RemapTblCompar);

    CFE_TBL_Modified(RemapTblHandle);

    RemapTbl = TblPtr;

    return SBN_SUCCESS;
} /* end LoadRemapTbl() */

/* finds the entry or the one that would immediately follow it */
static int BinarySearch(void *Entries, void *SearchEntry, size_t EntryCnt, size_t EntrySz,
                        int (*EntryCompare)(const void *, const void *))
{
    int start, end, midpoint, found;

    for (start = 0, end = EntryCnt - 1, found = 0; found == 0 && start <= end;)
    {
        midpoint = (end + start) / 2;
        int c    = EntryCompare(SearchEntry, (uint8 *)Entries + EntrySz * midpoint);
        if (c == 0)
        {
            return midpoint;
        }
        else if (c > 0)
        {
            start = midpoint + 1;
        }
        else
        {
            end = midpoint - 1;
        } /* end if */
    }     /* end while */

    if (found == 0)
    {
        return EntryCnt;
    } /* end if */

    return midpoint;
} /* end BinarySearch() */

static int RemapTblSearch(uint32 ProcessorID, CFE_SB_MsgId_t MID)
{
    SBN_RemapTblEntry_t Entry = {ProcessorID, MID, 0x0000};
    return BinarySearch(RemapTbl->Entries, &Entry, RemapTblCnt, sizeof(SBN_RemapTblEntry_t), RemapTblCompar);
} /* end RemapTblSearch() */

static SBN_Status_t Remap(void *msg, SBN_Filter_Ctx_t *Context)
{
    CFE_SB_MsgId_t     FromMID = 0x0000, ToMID = 0x0000;
    CFE_MSG_Message_t *CFE_MsgPtr = msg;

    if (CFE_MSG_GetMsgId(CFE_MsgPtr, &FromMID) != CFE_SUCCESS)
    {
        EVSSendErr(SBN_F_REMAP_EID, "unable to get apid");
        return SBN_ERROR;
    } /* end if */

    if (OS_MutSemTake(RemapMutex) != OS_SUCCESS)
    {
        EVSSendErr(SBN_F_REMAP_EID, "unable to take mutex");
        return SBN_ERROR;
    } /* end if */

    int i = RemapTblSearch(Context->PeerProcessorID, FromMID);

    if (i < RemapTblCnt && RemapTbl->Entries[i].ProcessorID == Context->PeerProcessorID &&
        RemapTbl->Entries[i].FromMID == FromMID)
    {
        ToMID = RemapTbl->Entries[i].ToMID;
    }
    else
    {
        if (RemapTbl->RemapDefaultFlag == SBN_REMAP_DEFAULT_SEND)
        {
            ToMID = FromMID;
        } /* end if */
    }     /* end if */

    if (OS_MutSemGive(RemapMutex) != OS_SUCCESS)
    {
        EVSSendErr(SBN_F_REMAP_EID, "unable to give mutex");
        return SBN_ERROR;
    } /* end if */

    if (ToMID == 0x0000)
    {
        return SBN_IF_EMPTY; /* signal to the core app that this filter recommends not sending this message */
    }                        /* end if */

    if (CFE_MSG_SetMsgId(CFE_MsgPtr, ToMID) != CFE_SUCCESS)
    {
        EVSSendErr(SBN_F_REMAP_EID, "unable to set msgid");
        return SBN_ERROR;
    } /* end if */

    return SBN_SUCCESS;
} /* end Remap() */

static SBN_Status_t Remap_MID(CFE_SB_MsgId_t *InOutMsgIdPtr, SBN_Filter_Ctx_t *Context)
{
    int i = 0;

    for (i = 0; i < RemapTblCnt; i++)
    {
        if (RemapTbl->Entries[i].ProcessorID == Context->PeerProcessorID &&
            RemapTbl->Entries[i].ToMID == *InOutMsgIdPtr)
        {
            *InOutMsgIdPtr = RemapTbl->Entries[i].FromMID;
            return SBN_SUCCESS;
        } /* end if */
    }     /* end for */

    return SBN_SUCCESS;
} /* end Remap_MID() */

static SBN_Status_t Init(int Version, CFE_EVS_EventID_t BaseEID)
{
    OS_Status_t OS_Status;

    SBN_F_REMAP_FIRST_EID = BaseEID;

    if (Version != 2) /* TODO: define */
    {
        OS_printf("SBN_F_Remap version mismatch: expected %d, got %d\n", 1, Version);
        return SBN_ERROR;
    } /* end if */

    OS_printf("SBN_F_Remap Lib Initialized.\n");

    OS_Status = OS_MutSemCreate(&RemapMutex, "SBN_F_Remap", 0);
    if (OS_Status != OS_SUCCESS)
    {
        return SBN_ERROR;
    } /* end if */

    return LoadRemapTbl();
} /* end Init() */

SBN_FilterInterface_t SBN_F_Remap = {Init, Remap, Remap, Remap_MID};
