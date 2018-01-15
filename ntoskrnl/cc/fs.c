/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/cc/fs.c
 * PURPOSE:         Implements cache managers functions useful for File Systems
 *
 * PROGRAMMERS:     Alex Ionescu
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifndef VACB_MAPPING_GRANULARITY
#define VACB_MAPPING_GRANULARITY (256 * 1024)
#endif

/* GLOBALS   *****************************************************************/

extern KGUARDED_MUTEX ViewLock;
extern ULONG DirtyPageCount;

NTSTATUS CcRosInternalFreeVacb(PROS_VACB Vacb);

/* FUNCTIONS *****************************************************************/

/*
 * @unimplemented
 */
LARGE_INTEGER
NTAPI
CcGetDirtyPages (
    IN PVOID LogHandle,
    IN PDIRTY_PAGE_ROUTINE DirtyPageRoutine,
    IN PVOID Context1,
    IN PVOID Context2)
{
    LARGE_INTEGER i;

    CCTRACE(CC_API_DEBUG, "LogHandle=%p DirtyPageRoutine=%p Context1=%p Context2=%p\n",
        LogHandle, DirtyPageRoutine, Context1, Context2);

    UNIMPLEMENTED;
    i.QuadPart = 0;
    return i;
}

/*
 * @implemented
 */
PFILE_OBJECT
NTAPI
CcGetFileObjectFromBcb (
    IN PVOID Bcb)
{
    PINTERNAL_BCB iBcb = (PINTERNAL_BCB)Bcb;

    CCTRACE(CC_API_DEBUG, "Bcb=%p\n", Bcb);

    return iBcb->Vacb->SharedCacheMap->FileObject;
}

/*
 * @unimplemented
 */
LARGE_INTEGER
NTAPI
CcGetLsnForFileObject (
    IN PFILE_OBJECT FileObject,
    OUT PLARGE_INTEGER OldestLsn OPTIONAL)
{
    LARGE_INTEGER i;

    CCTRACE(CC_API_DEBUG, "FileObject=%p\n", FileObject);

    UNIMPLEMENTED;
    i.QuadPart = 0;
    return i;
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcInitializeCacheMap (
    IN PFILE_OBJECT FileObject,
    IN PCC_FILE_SIZES FileSizes,
    IN BOOLEAN PinAccess,
    IN PCACHE_MANAGER_CALLBACKS CallBacks,
    IN PVOID LazyWriterContext)
{
    NTSTATUS Status;

    ASSERT(FileObject);
    ASSERT(FileSizes);

    CCTRACE(CC_API_DEBUG, "FileObject=%p FileSizes=%p PinAccess=%d CallBacks=%p LazyWriterContext=%p\n",
        FileObject, FileSizes, PinAccess, CallBacks, LazyWriterContext);

    /* Call old ROS cache init function */
    Status = CcRosInitializeFileCache(FileObject,
                                      FileSizes,
                                      PinAccess,
                                      CallBacks,
                                      LazyWriterContext);
    if (!NT_SUCCESS(Status))
        ExRaiseStatus(Status);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
CcIsThereDirtyData (
    IN PVPB Vpb)
{
    PROS_VACB Vacb;
    PLIST_ENTRY Entry;
    /* Assume no dirty data */
    BOOLEAN Dirty = FALSE;

    CCTRACE(CC_API_DEBUG, "Vpb=%p\n", Vpb);

    KeAcquireGuardedMutex(&ViewLock);

    /* Browse dirty VACBs */
    for (Entry = DirtyVacbListHead.Flink; Entry != &DirtyVacbListHead; Entry = Entry->Flink)
    {
        Vacb = CONTAINING_RECORD(Entry, ROS_VACB, DirtyVacbListEntry);
        /* Look for these associated with our volume */
        if (Vacb->SharedCacheMap->FileObject->Vpb != Vpb)
        {
            continue;
        }

        /* From now on, we are associated with our VPB */

        /* Temporary files are not counted as dirty */
        if (BooleanFlagOn(Vacb->SharedCacheMap->FileObject->Flags, FO_TEMPORARY_FILE))
        {
            continue;
        }

        /* A single dirty VACB is enough to have dirty data */
        if (Vacb->Dirty)
        {
            Dirty = TRUE;
            break;
        }
    }

    KeReleaseGuardedMutex(&ViewLock);

    return Dirty;
}

/*
 * @unimplemented
 */
BOOLEAN
NTAPI
CcPurgeCacheSection (
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN PLARGE_INTEGER FileOffset OPTIONAL,
    IN ULONG Length,
    IN BOOLEAN UninitializeCacheMaps)
{
    PROS_SHARED_CACHE_MAP SharedCacheMap;
    LONGLONG StartOffset;
    LONGLONG EndOffset;
    LIST_ENTRY FreeList;
    KIRQL OldIrql;
    PLIST_ENTRY ListEntry;
    PROS_VACB Vacb;
    LONGLONG ViewEnd;
    BOOLEAN Success;

    CCTRACE(CC_API_DEBUG, "SectionObjectPointer=%p\n FileOffset=%p Length=%lu UninitializeCacheMaps=%d",
        SectionObjectPointer, FileOffset, Length, UninitializeCacheMaps);

    if (UninitializeCacheMaps)
    {
        DPRINT1("FIXME: CcPurgeCacheSection not uninitializing private cache maps\n");
    }

    SharedCacheMap = SectionObjectPointer->SharedCacheMap;
    if (!SharedCacheMap)
        return FALSE;

    StartOffset = FileOffset != NULL ? FileOffset->QuadPart : 0;
    if (Length == 0 || FileOffset == NULL)
    {
        EndOffset = MAXLONGLONG;
    }
    else
    {
        EndOffset = StartOffset + Length;
        ASSERT(EndOffset > StartOffset);
    }

    InitializeListHead(&FreeList);

    /* Assume success */
    Success = TRUE;

    KeAcquireGuardedMutex(&ViewLock);
    KeAcquireSpinLock(&SharedCacheMap->CacheMapLock, &OldIrql);
    ListEntry = SharedCacheMap->CacheMapVacbListHead.Flink;
    while (ListEntry != &SharedCacheMap->CacheMapVacbListHead)
    {
        Vacb = CONTAINING_RECORD(ListEntry, ROS_VACB, CacheMapVacbListEntry);
        ListEntry = ListEntry->Flink;

        /* Skip VACBs outside the range, or only partially in range */
        if (Vacb->FileOffset.QuadPart < StartOffset)
        {
            continue;
        }
        ViewEnd = min(Vacb->FileOffset.QuadPart + VACB_MAPPING_GRANULARITY,
                      SharedCacheMap->SectionSize.QuadPart);
        if (ViewEnd >= EndOffset)
        {
            break;
        }

        /* Still in use, it cannot be purged, fail */
        if (Vacb->ReferenceCount != 0 && !Vacb->Dirty)
        {
            Success = FALSE;
            break;
        }

        /* This VACB is in range, so unlink it and mark for free */
        RemoveEntryList(&Vacb->VacbLruListEntry);
        if (Vacb->Dirty)
        {
            RemoveEntryList(&Vacb->DirtyVacbListEntry);
            DirtyPageCount -= VACB_MAPPING_GRANULARITY / PAGE_SIZE;
        }
        RemoveEntryList(&Vacb->CacheMapVacbListEntry);
        InsertHeadList(&FreeList, &Vacb->CacheMapVacbListEntry);
    }
    KeReleaseSpinLock(&SharedCacheMap->CacheMapLock, OldIrql);
    KeReleaseGuardedMutex(&ViewLock);

    while (!IsListEmpty(&FreeList))
    {
        Vacb = CONTAINING_RECORD(RemoveHeadList(&FreeList),
                                 ROS_VACB,
                                 CacheMapVacbListEntry);
        CcRosInternalFreeVacb(Vacb);
    }

    return Success;
}


/*
 * @implemented
 */
VOID NTAPI
CcSetFileSizes (
    IN PFILE_OBJECT FileObject,
    IN PCC_FILE_SIZES FileSizes)
{
    KIRQL oldirql;
    PROS_SHARED_CACHE_MAP SharedCacheMap;

    CCTRACE(CC_API_DEBUG, "FileObject=%p FileSizes=%p\n",
        FileObject, FileSizes);

    DPRINT("CcSetFileSizes(FileObject 0x%p, FileSizes 0x%p)\n",
           FileObject, FileSizes);
    DPRINT("AllocationSize %I64d, FileSize %I64d, ValidDataLength %I64d\n",
           FileSizes->AllocationSize.QuadPart,
           FileSizes->FileSize.QuadPart,
           FileSizes->ValidDataLength.QuadPart);

    SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;

    /*
     * It is valid to call this function on file objects that weren't
     * initialized for caching. In this case it's simple no-op.
     */
    if (SharedCacheMap == NULL)
        return;

    if (FileSizes->AllocationSize.QuadPart < SharedCacheMap->SectionSize.QuadPart)
    {
        CcPurgeCacheSection(FileObject->SectionObjectPointer,
                            &FileSizes->AllocationSize,
                            0,
                            FALSE);
    }

    KeAcquireSpinLock(&SharedCacheMap->CacheMapLock, &oldirql);
    SharedCacheMap->SectionSize = FileSizes->AllocationSize;
    SharedCacheMap->FileSize = FileSizes->FileSize;
    KeReleaseSpinLock(&SharedCacheMap->CacheMapLock, oldirql);
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcSetLogHandleForFile (
    IN PFILE_OBJECT FileObject,
    IN PVOID LogHandle,
    IN PFLUSH_TO_LSN FlushToLsnRoutine)
{
    CCTRACE(CC_API_DEBUG, "FileObject=%p LogHandle=%p FlushToLsnRoutine=%p\n",
        FileObject, LogHandle, FlushToLsnRoutine);

    UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
BOOLEAN
NTAPI
CcUninitializeCacheMap (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER TruncateSize OPTIONAL,
    IN PCACHE_UNINITIALIZE_EVENT UninitializeCompleteEvent OPTIONAL)
{
    NTSTATUS Status;
    PROS_SHARED_CACHE_MAP SharedCacheMap;
    KIRQL OldIrql;

    CCTRACE(CC_API_DEBUG, "FileObject=%p TruncateSize=%p UninitializeCompleteEvent=%p\n",
        FileObject, TruncateSize, UninitializeCompleteEvent);

    if (TruncateSize != NULL &&
        FileObject->SectionObjectPointer != NULL &&
        FileObject->SectionObjectPointer->SharedCacheMap != NULL)
    {
        SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;
        KeAcquireSpinLock(&SharedCacheMap->CacheMapLock, &OldIrql);
        if (SharedCacheMap->FileSize.QuadPart > TruncateSize->QuadPart)
        {
            SharedCacheMap->FileSize = *TruncateSize;
        }
        KeReleaseSpinLock(&SharedCacheMap->CacheMapLock, OldIrql);
        CcPurgeCacheSection(FileObject->SectionObjectPointer,
                            TruncateSize,
                            0,
                            FALSE);
    }

    Status = CcRosReleaseFileCache(FileObject);
    if (UninitializeCompleteEvent)
    {
        KeSetEvent(&UninitializeCompleteEvent->Event, IO_NO_INCREMENT, FALSE);
    }
    return NT_SUCCESS(Status);
}

BOOLEAN
NTAPI
CcGetFileSizes (
    IN PFILE_OBJECT FileObject,
    IN PCC_FILE_SIZES FileSizes)
{
    PROS_SHARED_CACHE_MAP SharedCacheMap;

    SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;

    if (!SharedCacheMap)
        return FALSE;

    FileSizes->AllocationSize = SharedCacheMap->SectionSize;
    FileSizes->FileSize = FileSizes->ValidDataLength = SharedCacheMap->FileSize;
    return TRUE;
}
