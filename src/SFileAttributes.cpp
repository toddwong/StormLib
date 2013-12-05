/*****************************************************************************/
/* SAttrFile.cpp                          Copyright (c) Ladislav Zezula 2007 */
/*---------------------------------------------------------------------------*/
/* Description:                                                              */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 12.06.04  1.00  Lad  The first version of SAttrFile.cpp                   */
/*****************************************************************************/

#define __STORMLIB_SELF__
#include "StormLib.h"
#include "StormCommon.h"

//-----------------------------------------------------------------------------
// Local structures

typedef struct _MPQ_ATTRIBUTES_HEADER
{
    DWORD dwVersion;                    // Version of the (attributes) file. Must be 100 (0x64)
    DWORD dwFlags;                      // See MPQ_ATTRIBUTE_XXXX

    // Followed by an array of CRC32
    // Followed by an array of file times
    // Followed by an array of MD5
    // Followed by an array of patch bits
} MPQ_ATTRIBUTES_HEADER, *PMPQ_ATTRIBUTES_HEADER;

//-----------------------------------------------------------------------------
// Local functions

static DWORD GetSizeOfAttributesFile(DWORD dwAttrFlags, DWORD dwFileTableSize)
{
    DWORD cbAttrFile = sizeof(MPQ_ATTRIBUTES_HEADER);

    // Calculate size of the (attributes) file
    if(dwAttrFlags & MPQ_ATTRIBUTE_CRC32)
        cbAttrFile += dwFileTableSize * sizeof(DWORD);
    if(dwAttrFlags & MPQ_ATTRIBUTE_FILETIME)
        cbAttrFile += dwFileTableSize * sizeof(ULONGLONG);
    if(dwAttrFlags & MPQ_ATTRIBUTE_MD5)
        cbAttrFile += dwFileTableSize * MD5_DIGEST_SIZE;
    
    // Weird: When there's 1 extra bit in the patch bit array, it's ignored
    // wow-update-13164.MPQ: BlockTableSize = 0x62E1, but there's only 0xC5C bytes
    if(dwAttrFlags & MPQ_ATTRIBUTE_PATCH_BIT)
        cbAttrFile += (dwFileTableSize + 6) / 8;
    
    return cbAttrFile;
}

static int LoadAttributesFile(TMPQArchive * ha, LPBYTE pbAttrFile, DWORD cbAttrFile)
{
    LPBYTE pbAttrFileEnd = pbAttrFile + cbAttrFile;
    LPBYTE pbAttrPtr = pbAttrFile;
    DWORD dwBlockTableSize = ha->pHeader->dwBlockTableSize;
    DWORD i;

    // Load and verify the header
    if((pbAttrPtr + sizeof(MPQ_ATTRIBUTES_HEADER)) <= pbAttrFileEnd)
    {
        PMPQ_ATTRIBUTES_HEADER pAttrHeader = (PMPQ_ATTRIBUTES_HEADER)pbAttrPtr;
        
        BSWAP_ARRAY32_UNSIGNED(pAttrHeader, sizeof(MPQ_ATTRIBUTES_HEADER));
        if(pAttrHeader->dwVersion != MPQ_ATTRIBUTES_V1)
            return ERROR_BAD_FORMAT;

        // Verify the flags and size of the file
        assert((pAttrHeader->dwFlags & ~MPQ_ATTRIBUTE_ALL) == 0);
        assert(GetSizeOfAttributesFile(pAttrHeader->dwFlags, dwBlockTableSize) == cbAttrFile);

        ha->dwAttrFlags = pAttrHeader->dwFlags;
        pbAttrPtr = (LPBYTE)(pAttrHeader + 1);
    }

    // Load the CRC32 (if present)
    if(ha->dwAttrFlags & MPQ_ATTRIBUTE_CRC32)
    {
        LPDWORD ArrayCRC32 = (LPDWORD)pbAttrPtr;
        DWORD cbArraySize = dwBlockTableSize * sizeof(DWORD);

        // Verify if there's enough data
        if((pbAttrPtr + cbArraySize) > pbAttrFileEnd)
            return ERROR_FILE_CORRUPT;

        BSWAP_ARRAY32_UNSIGNED(ArrayCRC32, cbCRC32Size);
        for(i = 0; i < dwBlockTableSize; i++)
            ha->pFileTable[i].dwCrc32 = ArrayCRC32[i];
        pbAttrPtr += cbArraySize;
    }

    // Load the FILETIME (if present)
    if(ha->dwAttrFlags & MPQ_ATTRIBUTE_FILETIME)
    {
        ULONGLONG * ArrayFileTime = (ULONGLONG *)pbAttrPtr;
        DWORD cbArraySize = dwBlockTableSize * sizeof(ULONGLONG);

        // Verify if there's enough data
        if((pbAttrPtr + cbArraySize) > pbAttrFileEnd)
            return ERROR_FILE_CORRUPT;

        BSWAP_ARRAY64_UNSIGNED(ArrayFileTime, cbFileTimeSize);
        for(i = 0; i < dwBlockTableSize; i++)
            ha->pFileTable[i].FileTime = ArrayFileTime[i];
        pbAttrPtr += cbArraySize;
    }

    // Load the MD5 (if present)
    if(ha->dwAttrFlags & MPQ_ATTRIBUTE_MD5)
    {
        LPBYTE ArrayMd5 = pbAttrPtr;
        DWORD cbArraySize = dwBlockTableSize * MD5_DIGEST_SIZE;

        // Verify if there's enough data
        if((pbAttrPtr + cbArraySize) > pbAttrFileEnd)
            return ERROR_FILE_CORRUPT;

        for(i = 0; i < dwBlockTableSize; i++)
        {
            memcpy(ha->pFileTable[i].md5, ArrayMd5, MD5_DIGEST_SIZE);
            ArrayMd5 += MD5_DIGEST_SIZE;
        }
        pbAttrPtr += cbArraySize;
    }

    // Read the patch bit for each file (if present)
    if(ha->dwAttrFlags & MPQ_ATTRIBUTE_PATCH_BIT)
    {
        LPBYTE pbBitArray = pbAttrPtr;
        DWORD cbArraySize = (dwBlockTableSize + 7) / 8;
        DWORD dwByteIndex = 0;
        DWORD dwBitMask = 0x80;

        // Verify if there's enough data
        if((pbAttrPtr + cbArraySize) > pbAttrFileEnd)
            return ERROR_FILE_CORRUPT;

        for(i = 0; i < dwBlockTableSize; i++)
        {
            ha->pFileTable[i].dwFlags |= (pbBitArray[dwByteIndex] & dwBitMask) ? MPQ_FILE_PATCH_FILE : 0;
            dwByteIndex += (dwBitMask & 0x01);
            dwBitMask = (dwBitMask << 0x07) | (dwBitMask >> 0x01);
        }

        pbAttrPtr += cbArraySize;
    }

    // 
    // Note: Version 7.00 of StormLib saved the (attributes) incorrectly. 
    // Sometimes, number of entries in the (attributes) was 1 item less
    // than block table size. 
    // If we encounter such table, we will zero all three arrays
    //

    if(pbAttrPtr != pbAttrFileEnd)
        ha->dwAttrFlags = 0;
    return ERROR_SUCCESS;
}

static LPBYTE CreateAttributesFile(TMPQArchive * ha, DWORD * pcbAttrFile)
{
    PMPQ_ATTRIBUTES_HEADER pAttrHeader;
    TFileEntry * pFileTableEnd = ha->pFileTable + ha->dwFileTableSize;
    TFileEntry * pFileEntry;
    LPBYTE pbAttrFile;
    LPBYTE pbAttrPtr;
    size_t cbAttrFile;
    DWORD dwFinalEntries = ha->dwFileTableSize + ha->dwReservedFiles;

    // Check if we need patch bits in the (attributes) file
    for(pFileEntry = ha->pFileTable; pFileEntry < pFileTableEnd; pFileEntry++)
    {
        if(pFileEntry->dwFlags & MPQ_FILE_PATCH_FILE)
        {
            ha->dwAttrFlags |= MPQ_ATTRIBUTE_PATCH_BIT;
            break;
        }
    }

    // Allocate the buffer for holding the entire (attributes)
    // Allodate 1 byte more (See GetSizeOfAttributesFile for more info)
    cbAttrFile = GetSizeOfAttributesFile(ha->dwAttrFlags, dwFinalEntries);
    pbAttrFile = pbAttrPtr = STORM_ALLOC(BYTE, cbAttrFile + 1);
    if(pbAttrFile != NULL)
    {
        // Make sure it's all zeroed
        memset(pbAttrFile, 0, cbAttrFile + 1);

        // Write the header of the (attributes) file
        pAttrHeader = (PMPQ_ATTRIBUTES_HEADER)pbAttrPtr;
        pAttrHeader->dwVersion = BSWAP_INT32_UNSIGNED(100);
        pAttrHeader->dwFlags   = BSWAP_INT32_UNSIGNED((ha->dwAttrFlags & MPQ_ATTRIBUTE_ALL));
        pbAttrPtr = (LPBYTE)(pAttrHeader + 1);

        // Write the array of CRC32, if present
        if(ha->dwAttrFlags & MPQ_ATTRIBUTE_CRC32)
        {
            LPDWORD pArrayCRC32 = (LPDWORD)pbAttrPtr;

            // Copy from file table
            for(pFileEntry = ha->pFileTable; pFileEntry < pFileTableEnd; pFileEntry++)
                *pArrayCRC32++ = BSWAP_INT32_UNSIGNED(pFileEntry->dwCrc32);

            // Skip the reserved entries
            pbAttrPtr = (LPBYTE)(pArrayCRC32 + ha->dwReservedFiles);
        }

        // Write the array of file time
        if(ha->dwAttrFlags & MPQ_ATTRIBUTE_FILETIME)
        {
            ULONGLONG * pArrayFileTime = (ULONGLONG *)pbAttrPtr;

            // Copy from file table
            for(pFileEntry = ha->pFileTable; pFileEntry < pFileTableEnd; pFileEntry++)
                *pArrayFileTime++ = BSWAP_INT64_UNSIGNED(pFileEntry->FileTime);

            // Skip the reserved entries
            pbAttrPtr = (LPBYTE)(pArrayFileTime + ha->dwReservedFiles);
        }

        // Write the array of MD5s
        if(ha->dwAttrFlags & MPQ_ATTRIBUTE_MD5)
        {
            LPBYTE pbArrayMD5 = pbAttrPtr;

            // Copy from file table
            for(pFileEntry = ha->pFileTable; pFileEntry < pFileTableEnd; pFileEntry++)
            {
                memcpy(pbArrayMD5, pFileEntry->md5, MD5_DIGEST_SIZE);
                pbArrayMD5 += MD5_DIGEST_SIZE;
            }

            // Skip the reserved items
            pbAttrPtr = pbArrayMD5 + (ha->dwReservedFiles * MD5_DIGEST_SIZE);
        }

        // Write the array of patch bits
        if(ha->dwAttrFlags & MPQ_ATTRIBUTE_PATCH_BIT)
        {
            LPBYTE pbBitArray = pbAttrPtr;
            DWORD dwByteSize = (dwFinalEntries + 7) / 8;
            DWORD dwByteIndex = 0;
            DWORD dwBitMask = 0x80;

            // Copy from file table
            for(pFileEntry = ha->pFileTable; pFileEntry < pFileTableEnd; pFileEntry++)
            {
                // Set the bit, if needed
                if(pFileEntry->dwFlags & MPQ_FILE_PATCH_FILE)
                    pbBitArray[dwByteIndex] |= dwBitMask;

                // Update bit index and bit mask
                dwByteIndex += (dwBitMask & 0x01);
                dwBitMask = (dwBitMask << 0x07) | (dwBitMask >> 0x01);
            }

            // Move past the bit array
            pbAttrPtr = (pbBitArray + dwByteSize);
        }

        // Now we expect that current position matches the estimated size
        // Note that if there is 1 extra bit above the byte size,
        // the table is actually 1 byte shorted in Blizzard MPQs. See GetSizeOfAttributesFile
        assert((size_t)(pbAttrPtr - pbAttrFile) == cbAttrFile + ((dwFinalEntries & 0x07) == 1) ? 1 : 0);
    }

    // Give away the attributes file
    if(pcbAttrFile != NULL)
        *pcbAttrFile = (DWORD)cbAttrFile;
    return pbAttrFile;
}

//-----------------------------------------------------------------------------
// Public functions (internal use by StormLib)

int SAttrLoadAttributes(TMPQArchive * ha)
{
    HANDLE hFile = NULL;
    LPBYTE pbAttrFile;
    DWORD dwBytesRead;
    DWORD cbAttrFile = 0;
    int nError = ERROR_FILE_CORRUPT;

    // File table must be initialized
    assert(ha->pFileTable != NULL);

    // Attempt to open the "(attributes)" file.
    // If it's not there, then the archive doesn't support attributes
    if(SFileOpenFileEx((HANDLE)ha, ATTRIBUTES_NAME, SFILE_OPEN_ANY_LOCALE, &hFile))
    {
        // Retrieve and check size of the (attributes) file
        cbAttrFile = SFileGetFileSize(hFile, NULL);

        // Size of the (attributes) might be 1 byte less than expected
        // See GetSizeOfAttributesFile for more info
        pbAttrFile = STORM_ALLOC(BYTE, cbAttrFile + 1);
        if(pbAttrFile != NULL)
        {
            // Set the last byte to 0 in case the size should be 1 byte greater
            pbAttrFile[cbAttrFile] = 0;

            // Load the entire file to memory
            SFileReadFile(hFile, pbAttrFile, cbAttrFile, &dwBytesRead, NULL);
            if(dwBytesRead == cbAttrFile)
                nError = LoadAttributesFile(ha, pbAttrFile, cbAttrFile);

            // Free the buffer
            STORM_FREE(pbAttrFile);
        }

        // Close the attributes file
        SFileCloseFile(hFile);
    }

    return nError;
}

// Saves the (attributes) to the MPQ
int SAttrFileSaveToMpq(TMPQArchive * ha)
{
    TMPQFile * hf = NULL;
    LPBYTE pbAttrFile;
    DWORD cbAttrFile = 0;
    int nError = ERROR_SUCCESS;

    // If there are no file flags for (attributes) file, do nothing
    if(ha->dwFileFlags2 == 0 || ha->dwMaxFileCount == 0)
        return ERROR_SUCCESS;

    // We expect at least one reserved entry to be there
    assert(ha->dwReservedFiles >= 1);

    // Create the raw data that is to be written to (attributes)
    // Note: Blizzard MPQs have entries for (listfile) and (attributes),
    // but they are filled empty
    pbAttrFile = CreateAttributesFile(ha, &cbAttrFile);

    // Now we decrement the number of reserved files.
    // This frees one slot in the file table, so the subsequent file create operation should succeed
    // This must happen even if CreateAttributesFile failed
    ha->dwReservedFiles--;

    // If we created something, write the attributes to the MPQ
    if(pbAttrFile != NULL)
    {
        // We expect it to be nonzero size
        assert(cbAttrFile != 0);

        // Determine the real flags for (attributes)
        if(ha->dwFileFlags2 == MPQ_FILE_EXISTS)
            ha->dwFileFlags2 = GetDefaultSpecialFileFlags(cbAttrFile, ha->pHeader->wFormatVersion);

        // Create the attributes file in the MPQ
        nError = SFileAddFile_Init(ha, ATTRIBUTES_NAME,
                                       0,
                                       cbAttrFile,
                                       LANG_NEUTRAL,
                                       ha->dwFileFlags2 | MPQ_FILE_REPLACEEXISTING,
                                      &hf);

        // Write the attributes file raw data to it
        if(nError == ERROR_SUCCESS)
        {
            // Write the content of the attributes file to the MPQ
            nError = SFileAddFile_Write(hf, pbAttrFile, cbAttrFile, MPQ_COMPRESSION_ZLIB);
            SFileAddFile_Finish(hf);

            // Clear the invalidate flag
            ha->dwFlags &= ~MPQ_FLAG_ATTRIBUTES_INVALID;
        }

        // Free the attributes buffer
        STORM_FREE(pbAttrFile);
    }
    
    return nError;
}

//-----------------------------------------------------------------------------
// Public functions

DWORD WINAPI SFileGetAttributes(HANDLE hMpq)
{
    TMPQArchive * ha = (TMPQArchive *)hMpq;

    // Verify the parameters
    if(!IsValidMpqHandle(hMpq))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return SFILE_INVALID_ATTRIBUTES;
    }

    return ha->dwAttrFlags;
}

bool WINAPI SFileSetAttributes(HANDLE hMpq, DWORD dwFlags)
{
    TMPQArchive * ha = (TMPQArchive *)hMpq;

    // Verify the parameters
    if(!IsValidMpqHandle(hMpq))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    // Not allowed when the archive is read-only
    if(ha->dwFlags & MPQ_FLAG_READ_ONLY)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }

    // Set the attributes
    InvalidateInternalFiles(ha);
    ha->dwAttrFlags = (dwFlags & MPQ_ATTRIBUTE_ALL);
    return true;
}

bool WINAPI SFileUpdateFileAttributes(HANDLE hMpq, const char * szFileName)
{
    hash_state md5_state;
    TMPQArchive * ha = (TMPQArchive *)hMpq;
    TMPQFile * hf;
    BYTE Buffer[0x1000];
    HANDLE hFile = NULL;
    DWORD dwTotalBytes = 0;
    DWORD dwBytesRead;
    DWORD dwCrc32;

    // Verify the parameters
    if(!IsValidMpqHandle(ha))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    // Not allowed when the archive is read-only
    if(ha->dwFlags & MPQ_FLAG_READ_ONLY)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }

    // Attempt to open the file
    if(!SFileOpenFileEx(hMpq, szFileName, SFILE_OPEN_BASE_FILE, &hFile))
        return false;

    // Get the file size
    hf = (TMPQFile *)hFile;
    dwTotalBytes = hf->pFileEntry->dwFileSize;

    // Initialize the CRC32 and MD5 contexts
    md5_init(&md5_state);
    dwCrc32 = crc32(0, Z_NULL, 0);

    // Go through entire file and calculate both CRC32 and MD5
    while(dwTotalBytes != 0)
    {
        // Read data from file
        SFileReadFile(hFile, Buffer, sizeof(Buffer), &dwBytesRead, NULL);
        if(dwBytesRead == 0)
            break;

        // Update CRC32 and MD5
        dwCrc32 = crc32(dwCrc32, Buffer, dwBytesRead);
        md5_process(&md5_state, Buffer, dwBytesRead);

        // Decrement the total size
        dwTotalBytes -= dwBytesRead;
    }

    // Update both CRC32 and MD5
    hf->pFileEntry->dwCrc32 = dwCrc32;
    md5_done(&md5_state, hf->pFileEntry->md5);

    // Remember that we need to save the MPQ tables
    InvalidateInternalFiles(ha);
    SFileCloseFile(hFile);
    return true;
}
