/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
// Original file Copyright Crytek GMBH or its affiliates, used under license.

// Description: Implementation of the IPlatformOS interface for PC

#include <StdAfx.h>

#if defined(AZ_RESTRICTED_PLATFORM)
    #if defined(AZ_PLATFORM_XENIA)
        #include "Xenia/PlatformOS_PC_cpp_xenia.inl"
    #elif defined(AZ_PLATFORM_PROVO)
        #include "Provo/PlatformOS_PC_cpp_provo.inl"
    #endif
#endif
#if defined(AZ_RESTRICTED_SECTION_IMPLEMENTED)
#undef AZ_RESTRICTED_SECTION_IMPLEMENTED
#else

#if defined(WIN32) || defined(WIN64)
#define PLATFORM_WINDOWS
#endif

#include "PlatformOS_PC.h"
#include "SaveReaderWriter_CryPak.h"
#include "ICryPak.h"

#include "../ZipDir.h"
#include "../CryArchive.h"
#include "ICrypto.h"
#include <IZLibCompressor.h>
#include <StringUtils.h>

#if USE_STEAM
#include "Steamworks/public/steam/steam_api.h"
#include "SaveReaderWriter_Steam.h"
#endif

#include "NativeUIRequests.h"

IPlatformOS* IPlatformOS::Create(const uint8 createParams)
{
    return new CPlatformOS_PC(createParams);
}

CPlatformOS_PC::CPlatformOS_PC(const uint8 createParams)
    : m_listeners(4)
    , m_fpsWatcher(15.0f, 3.0f, 7.0f)
    , m_delayLevelStartIcon(0.0f)
    , m_bSaving(false)
#if defined(DEDICATED_SERVER)
    , m_bAllowMessageBox(false)
#else
    , m_bAllowMessageBox((createParams & eCF_NoDialogs) == 0)
#endif //defined(DEDICATED_SERVER)
    , m_bLevelLoad(false)
    , m_bSaveDuringLevelLoad(false)
{
    AddListener(this, "PC");

    m_cachePakStatus = 0;
    m_cachePakUser = -1;
    //TODO: Handle early corruption detection (createParams & IPlatformOS::eCF_EarlyCorruptionDetected ) if necessary.
}

void CPlatformOS_PC::Tick(float realFrameTime)
{
    if (m_delayLevelStartIcon)
    {
        m_delayLevelStartIcon -= realFrameTime;
        if (m_delayLevelStartIcon <= 0.0f)
        {
            m_delayLevelStartIcon = 0.0f;

            IPlatformOS::SPlatformEvent event(0);
            event.m_eEventType = IPlatformOS::SPlatformEvent::eET_FileWrite;
            event.m_uParams.m_fileWrite.m_type = SPlatformEvent::eFWT_SaveStart;
            NotifyListeners(event);

            event.m_eEventType = IPlatformOS::SPlatformEvent::eET_FileWrite;
            event.m_uParams.m_fileWrite.m_type = SPlatformEvent::eFWT_SaveEnd;
            NotifyListeners(event);
        }
    }

    SaveDirtyFiles();
}

void CPlatformOS_PC::OnPlatformEvent(const IPlatformOS::SPlatformEvent& _event)
{
    switch (_event.m_eEventType)
    {
    case SPlatformEvent::eET_FileWrite:
    {
        if (_event.m_uParams.m_fileWrite.m_type == SPlatformEvent::eFWT_CheckpointLevelStart)
        {
            m_delayLevelStartIcon = 5.0f;
        }
        break;
    }
    }
}

// func returns true if the data is correctly encrypted and signed. caller is then responsible for calling delete[] on the returned data buffer
// returns false if there is a problem, caller has no data to free if false is returned
bool CPlatformOS_PC::DecryptAndCheckSigning(const char* pInData, int inDataLen, char** pOutData, int* pOutDataLen, const uint8 key[16])
{
    ICrypto* pCrypto = gEnv->pSystem->GetCrypto();
    IZLibCompressor* pZLib = GetISystem()->GetIZLibCompressor();
    StreamCipherState               cipher = pCrypto->GetStreamCipher()->BeginCipher(key, 8); // Use the first 8 bytes of the key as the decryption key
    char* pOutput = NULL;
    int                             outDataLen = 0;
    bool                            valid = false;

    if (inDataLen > 16)
    {
        {
            pOutput = new char[inDataLen];
            if (pOutput)
            {
                pCrypto->GetStreamCipher()->Decrypt(cipher, (const uint8*) pInData, inDataLen, (uint8*) pOutput);

                // validate cksum to check for successful decryption and for data signing
                SMD5Context     ctx;
                char                    digest[16];

                pZLib->MD5Init(&ctx);
                pZLib->MD5Update(&ctx, (const char*)(key + 8), 8); // Use the last 8 bytes of the key as the signing key
                pZLib->MD5Update(&ctx, pOutput, inDataLen - 16);
                pZLib->MD5Final(&ctx, digest);

                if (memcmp(digest, pOutput + inDataLen - 16, 16) != 0)
                {
                    CryLog("MD5 fail on dlc xml");
                }
                else
                {
                    CryLog("dlc xml passed decrypt and MD5 checks");
                    valid = true;
                }
            }
        }
    }

    if (valid)
    {
        *pOutData = pOutput;
        *pOutDataLen = inDataLen - 16;
    }
    else
    {
        delete [] pOutput;
        *pOutData = NULL;
        *pOutDataLen = 0;
    }

    return valid;
}

void CPlatformOS_PC::MountDLCContent(IDLCListener* pCallback, unsigned int user, const uint8 keyData[16])
{
    //Get the DLC install directory
    const char* dlcDir = "$DLC$";
    int nDLCPacksFound = 0;

    ICryPak* pCryPak = gEnv->pCryPak;

    if (pCryPak->GetAlias(dlcDir) == NULL)
    {
        pCallback->OnDLCMountFailed(eDMF_NoDLCDir);
    }
    else
    {
        CryFixedStringT<ICryPak::g_nMaxPath> findPath;
        findPath.Format("%s/*", dlcDir);

        CryLog("Passing %s to File Finder (dlcDir %s", findPath.c_str(), dlcDir);

        // Search for all subfolders with a file named dlc.sxml, this corresponds to a DLC package
        IFileFinderPtr fileFinder = GetFileFinder(user);
        _finddata_t fd;
        intptr_t nFS = fileFinder->FindFirst(findPath.c_str(), &fd);
        if (nFS != -1)
        {
            do
            {
                // Skip files, only want subdirectories
                if (!(fd.attrib & _A_SUBDIR) || !strcmp(fd.name, ".") || !strcmp(fd.name, ".."))
                {
                    continue;
                }

                // Try to load the xml file
                stack_string path;
                path.Format("%s/%s/dlc.sxml", dlcDir, fd.name);

                CryLog("DLC: Trying %s", path.c_str());

                AZ::IO::HandleType fileHandle = pCryPak->FOpen(path.c_str(), "rb", ICryPak::FOPEN_HINT_QUIET | ICryPak::FOPEN_ONDISK | ICryArchive::FLAGS_ABSOLUTE_PATHS);
                if (fileHandle != AZ::IO::InvalidHandle)
                {
                    CryLog("DLC: Found %s", path.c_str());
                    bool success = false;
                    pCryPak->FSeek(fileHandle, 0, SEEK_END);
                    int size = pCryPak->FTell(fileHandle);
                    pCryPak->FSeek(fileHandle, 0, SEEK_SET);
                    char* pData = new char[size];
                    pCryPak->FRead(pData, size, fileHandle);
                    pCryPak->FClose(fileHandle);

                    char* pDecryptedData = NULL;
                    int decryptedSize = 0;
                    if (DecryptAndCheckSigning(pData, size, &pDecryptedData, &decryptedSize, keyData))
                    {
                        XmlNodeRef rootNode = gEnv->pSystem->LoadXmlFromBuffer(pDecryptedData, decryptedSize);
                        if (rootNode)
                        {
                            path.Format("%s/%s", dlcDir, fd.name);
                            pCallback->OnDLCMounted(rootNode, path.c_str());
                            success = true;

                            nDLCPacksFound++;
                        }
                        delete [] pDecryptedData;
                    }
                    delete [] pData;
                    if (!success)
                    {
                        pCallback->OnDLCMountFailed(eDMF_XmlError);
                    }
                }
            } while (0 == fileFinder->FindNext(nFS, &fd));
            fileFinder->FindClose(nFS);
        }
    }

    pCallback->OnDLCMountFinished(nDLCPacksFound);
}

bool CPlatformOS_PC::CanRestartTitle() const
{
    return false;
}

void CPlatformOS_PC::RestartTitle(const char* pTitle)
{
    CRY_ASSERT_MESSAGE(CanRestartTitle(), "Restart title not implemented (or previously needed)");
}

bool CPlatformOS_PC::UsePlatformSavingAPI() const
{
    // Default true if CVar doesn't exist
    ICVar* pUseAPI = gEnv->pConsole ? gEnv->pConsole->GetCVar("sys_usePlatformSavingAPI") : NULL;
    return !pUseAPI || pUseAPI->GetIVal() != 0;
}

bool CPlatformOS_PC::BeginSaveLoad(unsigned int user, bool bSave)
{
    m_bSaving = bSave;
    return true;
}

void CPlatformOS_PC::EndSaveLoad(unsigned int user)
{
}

bool CPlatformOS_PC::UseSteamReadWriter() const
{
#if USE_STEAM
    ICVar* pUseCloud = gEnv->pConsole ? gEnv->pConsole->GetCVar("sys_useSteamCloudForPlatformSaving") : NULL;
    if (pUseCloud && pUseCloud->GetIVal() != 0)
    {
        if (gEnv->pSystem && gEnv->pSystem->SteamInit())
        {
            if (SteamRemoteStorage()->IsCloudEnabledForAccount() && SteamRemoteStorage()->IsCloudEnabledForApp())
            {
                return true;
            }
        }
    }
#endif // USE_STEAM
    return false;
}

IPlatformOS::ISaveReaderPtr CPlatformOS_PC::SaveGetReader(const char* fileName, unsigned int /*user*/)
{
#if USE_STEAM
    if (UseSteamReadWriter())
    {
        CSaveReader_SteamPtr    pSaveReader(new CSaveReader_Steam(fileName));

        if (!pSaveReader || pSaveReader->LastError() != IPlatformOS::eFOC_Success)
        {
            return CSaveReader_SteamPtr();
        }
        else
        {
            return pSaveReader;
        }
    }
    else
#endif // USE_STEAM
    {
        CSaveReader_CryPakPtr   pSaveReader(new CSaveReader_CryPak(fileName));

        if (!pSaveReader || pSaveReader->LastError() != IPlatformOS::eFOC_Success)
        {
            return CSaveReader_CryPakPtr();
        }
        else
        {
            return pSaveReader;
        }
    }
}

IPlatformOS::ISaveWriterPtr CPlatformOS_PC::SaveGetWriter(const char* fileName, unsigned int /*user*/)
{
#if USE_STEAM
    if (UseSteamReadWriter())
    {
        CSaveWriter_SteamPtr    pSaveWriter(new CSaveWriter_Steam(fileName));

        if (!pSaveWriter || pSaveWriter->LastError() != IPlatformOS::eFOC_Success)
        {
            return CSaveWriter_SteamPtr();
        }
        else
        {
            if (m_bLevelLoad)
            {
                m_bSaveDuringLevelLoad = true;
            }

            return pSaveWriter;
        }
    }
    else
#endif
    {
        CSaveWriter_CryPakPtr   pSaveWriter(new CSaveWriter_CryPak(fileName));

        if (!pSaveWriter || pSaveWriter->LastError() != IPlatformOS::eFOC_Success)
        {
            return CSaveWriter_CryPakPtr();
        }
        else
        {
            if (m_bLevelLoad)
            {
                m_bSaveDuringLevelLoad = true;
            }

            return pSaveWriter;
        }
    }
}


void CPlatformOS_PC::InitEncryptionKey(const char* pMagic, size_t magicLength, const uint8* pKey, size_t keyLength)
{
    m_encryptionMagic.resize(magicLength);
    memcpy(&m_encryptionMagic[0], pMagic, magicLength);
    m_encryptionKey.resize(keyLength);
    memcpy(&m_encryptionKey[0], pKey, keyLength);
}

void CPlatformOS_PC::GetEncryptionKey(const DynArray<char>** pMagic, const DynArray<uint8>** pKey)
{
    if (pMagic)
    {
        *pMagic = &m_encryptionMagic;
    }
    if (pKey)
    {
        *pKey = &m_encryptionKey;
    }
}

/*
--------------------- Listener -----------------------
*/

void CPlatformOS_PC::AddListener(IPlatformOS::IPlatformListener* pListener, const char* szName)
{
    m_listeners.Add(pListener, szName);
}

IPlatformOS::EMsgBoxResult
CPlatformOS_PC::DebugMessageBox(const char* body, const char* title, unsigned int flags) const
{
    if (!m_bAllowMessageBox)
    {
        return eMsgBox_OK;
    }

    ICVar* pCVar = gEnv->pConsole ? gEnv->pConsole->GetCVar("sys_no_crash_dialog") : NULL;
    if (pCVar && pCVar->GetIVal() != 0)
    {
        return eMsgBox_OK;
    }


#if defined(AZ_PLATFORM_WINDOWS) || defined(AZ_PLATFORM_APPLE_OSX)
    int winresult = CryMessageBox(body, title, MB_OKCANCEL);
    return (winresult == IDOK) ? eMsgBox_OK : eMsgBox_Cancel;
#else
    EBUS_EVENT(NativeUI::NativeUIRequestBus, DisplayOkDialog, title, body, false);

    return eMsgBox_OK; // [AlexMcC|30.03.10]: Ok? Cancel? Dunno! Uh-oh :( This is only used in CryPak.cpp so far, and for that use it's better to return ok
#endif
}

bool CPlatformOS_PC::PostLocalizationBootChecks()
{
    //Not currently implemented
    return true;
}


void CPlatformOS_PC::SetOpticalDriveIdle(bool bIdle)
{
    //Not currently implemented
}
void CPlatformOS_PC::AllowOpticalDriveUsage(bool bAllow)
{
    //Not currently implemented
}

void CPlatformOS_PC::RemoveListener(IPlatformOS::IPlatformListener* pListener)
{
    m_listeners.Remove(pListener);
}

void CPlatformOS_PC::NotifyListeners(SPlatformEvent& event)
{
    for (CListenerSet<IPlatformOS::IPlatformListener*>::Notifier notifier(m_listeners); notifier.IsValid(); notifier.Next())
    {
        notifier->OnPlatformEvent(event);
    }
}

bool CPlatformOS_PC::StringVerifyStart(const char* inString, IStringVerifyEvents* pInCallback)
{
    return false;
}

bool CPlatformOS_PC::IsVerifyingString()
{
    return false;
}

ILocalizationManager::EPlatformIndependentLanguageID CPlatformOS_PC::GetSystemLanguage()
{
    //Not yet implemented
    return ILocalizationManager::ePILID_MAX_OR_INVALID;
}

const char* CPlatformOS_PC::GetSKUId()
{
    //Not yet implemented
    return NULL;
}

ILocalizationManager::TLocalizationBitfield CPlatformOS_PC::GetSystemSupportedLanguages()
{
    //Not yet implemented
    return 0;
}

void CPlatformOS_PC::GetMemoryUsage(ICrySizer* pSizer) const
{
    pSizer->Add(*this);
    m_listeners.GetMemoryUsage(pSizer);
}

bool CPlatformOS_PC::DebugSave(SDebugDump& dump)
{
    return false;
}

bool CPlatformOS_PC::ConsoleLoadGame(struct IConsoleCmdArgs* pArgs)
{
    return false;
}

EUserPIIStatus CPlatformOS_PC::GetUserPII(unsigned int inUser, SUserPII* pOutPII)
{
    return k_pii_error;
}

IPlatformOS::EZipExtractFail CPlatformOS_PC::ExtractZips(const char* path)
{
    IPlatformOS::EZipExtractFail retVal = eZEF_Success;
    CryLog("DLCZip: Extracting Downloaded zips");

    //get the path to the DLC install directory

    char userPath[ICryPak::g_nMaxPath];
    gEnv->pCryPak->AdjustFileName(path, userPath, AZ_ARRAY_SIZE(userPath), 0);

    //look for zips
    IPlatformOS::IFileFinderPtr fileFinder = GetFileFinder(0);
    _finddata_t fd;
    intptr_t nFS = fileFinder->FindFirst("@user@/DLC/*", &fd);
    if (nFS != -1)
    {
        do
        {
            stack_string filePath;
            filePath.Format("%s/%s", userPath, fd.name);

            // Skip dirs, only want files, and want them to be zips
            if ((fd.attrib == _A_SUBDIR) || strstr(fd.name, ".zip") == 0)
            {
                continue;
            }

            ICryArchive* pArc = gEnv->pCryPak->OpenArchive(filePath.c_str());

            if (pArc != NULL)
            {
                //find the files in the archive...
                ZipDir::CacheRWPtr pZip = static_cast<CryArchiveRW*>(pArc)->GetCache();
                ZipDir::FileEntryTree* pZipRoot = pZip->GetRoot();

                if (SxmlMissingFromHDD(pZipRoot, userPath, pZip))
                {
                    retVal = RecurseZipContents(pZipRoot, userPath, pZip);
                }
            }
        } while (0 == fileFinder->FindNext(nFS, &fd) && retVal == eZEF_Success);

        fileFinder->FindClose(nFS);
    }

    CryLog("DLCZip: Finished Extracting zips");

    return retVal;
}

IPlatformOS::EZipExtractFail CPlatformOS_PC::RecurseZipContents(ZipDir::FileEntryTree* pSourceDir, const char* currentPath, ZipDir::CacheRWPtr pCache)
{
    EZipExtractFail retVal = eZEF_Success;

    ZipDir::FileEntryTree::FileMap::iterator fileIt = pSourceDir->GetFileBegin();
    ZipDir::FileEntryTree::FileMap::iterator fileEndIt = pSourceDir->GetFileEnd();

    //look for files and output them to disk
    CryFixedStringT<ICryPak::g_nMaxPath> filePath;
    for (; fileIt != fileEndIt && retVal == eZEF_Success; fileIt++)
    {
        ZipDir::FileEntry* pFileEntry = pSourceDir->GetFileEntry(fileIt);
        const char* pFileName = pSourceDir->GetFileName(fileIt);


        filePath.Format("%s/%s", currentPath, pFileName);

        void* pData = pCache->AllocAndReadFile(pFileEntry);

        AZ::IO::HandleType newFileHandle = gEnv->pCryPak->FOpen(filePath.c_str(), "wb");
        if (newFileHandle == AZ::IO::InvalidHandle)
        {
            retVal = eZEF_WriteFail;
        }
        else
        {
            size_t written = gEnv->pCryPak->FWrite(pData, 1, pFileEntry->desc.lSizeUncompressed, newFileHandle);

            if (pFileEntry->desc.lSizeUncompressed != written)
            {
                //failed to extract file from zip, report error
                //drop out as fast and cleanly as we can
                retVal = eZEF_WriteFail;
            }

            gEnv->pCryPak->FClose(newFileHandle);
        }

        pCache->Free(pData);
    }

    ZipDir::FileEntryTree::SubdirMap::iterator dirIt = pSourceDir->GetDirBegin();
    ZipDir::FileEntryTree::SubdirMap::iterator dirEndIt = pSourceDir->GetDirEnd();

    //look for deeper directories in the heirarchy
    CryFixedStringT<ICryPak::g_nMaxPath> dirPath;
    for (; dirIt != dirEndIt && retVal == eZEF_Success; dirIt++)
    {
        dirPath.Format("%s/%s", currentPath, pSourceDir->GetDirName(dirIt));
        gEnv->pCryPak->MakeDir(dirPath);

        retVal = RecurseZipContents(pSourceDir->GetDirEntry(dirIt), dirPath.c_str(), pCache);
    }

    return retVal;
}


bool CPlatformOS_PC::SxmlMissingFromHDD(ZipDir::FileEntryTree* pZipRoot, const char* userPath, ZipDir::CacheRWPtr pZip)
{
    //return true if sxml is present in zip but not at equivalent path on HDD
    bool sxmlInZip = false;
    bool sxmlOnHDD = false;

    //sxml exists inside one of the dirs in the root of the zip

    ZipDir::FileEntryTree::SubdirMap::iterator dirIt = pZipRoot->GetDirBegin();
    ZipDir::FileEntryTree::SubdirMap::iterator dirEndIt = pZipRoot->GetDirEnd();
    CryFixedStringT<ICryPak::g_nMaxPath> dirPath;
    for (; dirIt != dirEndIt; dirIt++)
    {
        dirPath.Format("%s/%s", userPath, pZipRoot->GetDirName(dirIt));
        //gEnv->pCryPak->MakeDir( dirPath );

        ZipDir::FileEntryTree* pSourceDir = pZipRoot->GetDirEntry(dirIt);

        ZipDir::FileEntryTree::FileMap::iterator fileIt = pSourceDir->GetFileBegin();
        ZipDir::FileEntryTree::FileMap::iterator fileEndIt = pSourceDir->GetFileEnd();


        //look for files
        CryFixedStringT<ICryPak::g_nMaxPath> filePath;
        for (; fileIt != fileEndIt; fileIt++)
        {
            ZipDir::FileEntry* pFileEntry = pSourceDir->GetFileEntry(fileIt);
            const char* pFileName = pSourceDir->GetFileName(fileIt);

            if (strstr(pFileName, ".sxml"))
            {
                sxmlInZip = true;
                filePath.Format("%s/%s", dirPath.c_str(), pFileName);

                //this is the path where the sxml should be, is it present correctly?

                ICryPak* pCryPak = gEnv->pCryPak;
                AZ::IO::HandleType fileHandle = pCryPak->FOpen(filePath.c_str(), "rb", ICryPak::FOPEN_HINT_QUIET | ICryPak::FOPEN_ONDISK | ICryArchive::FLAGS_ABSOLUTE_PATHS);
                if (fileHandle != AZ::IO::InvalidHandle)
                {
                    sxmlOnHDD = true;
                }
            }
        }
    }

    if (sxmlInZip == true && sxmlOnHDD == false)
    {
        return true;
    }
    else
    {
        //either zip doesn't have an sxml (it's not a dlc zip) or has already been correctly extracted to disk
        return false;
    }
}

void CPlatformOS_PC::OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam)
{
    PlatformOS_Base::OnSystemEvent(event, wparam, lparam);

    switch (event)
    {
    case ESYSTEM_EVENT_LEVEL_LOAD_START:
        m_bLevelLoad = true;
        m_bSaveDuringLevelLoad = false;
        break;
    }
}

void CPlatformOS_PC::OnActionEvent(const SActionEvent& event)
{
    switch (event.m_event)
    {
    case eAE_mapCmdIssued:
        m_bLevelLoad = true;
        m_bSaveDuringLevelLoad = false;
        break;

    case eAE_inGame:
        m_bLevelLoad = false;
        m_fpsWatcher.Reset();
        break;
    }
}

void CPlatformOS_PC::SaveDirtyFiles()
{
    if (m_bSaveDuringLevelLoad)
    {
        // Wait for level load to finish
        if (m_bLevelLoad)
        {
            m_fpsWatcher.Reset();
            return;
        }

        if (!m_fpsWatcher.HasAchievedStableFPS())
        {
            return;
        }

        m_bSaveDuringLevelLoad = false;
        m_bLevelLoad = false;
    }

    if (m_bSaving)
    {
        if (!m_delayLevelStartIcon)
        {
            IPlatformOS::SPlatformEvent event(0);
            event.m_eEventType = IPlatformOS::SPlatformEvent::eET_FileWrite;
            event.m_uParams.m_fileWrite.m_type = SPlatformEvent::eFWT_SaveStart;
            NotifyListeners(event);
        }
        if (m_bSaving && !m_delayLevelStartIcon)
        {
            IPlatformOS::SPlatformEvent event(0);
            event.m_eEventType = IPlatformOS::SPlatformEvent::eET_FileWrite;
            event.m_uParams.m_fileWrite.m_type = IPlatformOS::SPlatformEvent::eFWT_SaveEnd;
            NotifyListeners(event);
            m_bSaving = false;
        }
    }
}

IPlatformOS::ECDP_Start CPlatformOS_PC::StartUsingCachePaks(const int user, bool* outWritesOccurred)
{
    if (outWritesOccurred)
    {
        *outWritesOccurred = false;
    }

    if (m_cachePakStatus != 0)
    {
        CryLog("StartUsingCachePak '%d' ERROR already in use", user);
        return IPlatformOS::eCDPS_AlreadyInUse;
    }

    m_cachePakStatus = 1;
    m_cachePakUser = user;

    return IPlatformOS::eCDPS_Success;
}

IPlatformOS::ECDP_End CPlatformOS_PC::EndUsingCachePaks(const int user)
{
    const int currentCachePakStatus = m_cachePakStatus;
    const int currentCachePakUser = m_cachePakUser;
    m_cachePakStatus = 0;
    m_cachePakUser = -1;

    if (currentCachePakStatus != 1)
    {
        CryLog("EndUsingCachePaks '%d' ERROR not in use", user);
        return IPlatformOS::eCDPE_NotInUse;
    }

    if (user != currentCachePakUser)
    {
        CryLog("EndUsingCachePaks '%d' ERROR wrong user : current cache pak user:%d", user, currentCachePakUser);
        return IPlatformOS::eCDPE_WrongUser;
    }

    return IPlatformOS::eCDPE_Success;
}

IPlatformOS::ECDP_Open CPlatformOS_PC::DoesCachePakExist(const char* const filename, const size_t size, unsigned char md5[16])
{
    if (m_cachePakStatus != 1)
    {
        CryLog("OpenCachePak '%s' ERROR cache paks not ready to be used", filename);
        return IPlatformOS::eCDPO_NotInUse;
    }

    bool ret;
    unsigned char fileMD5[16];

    CryFixedStringT<128> realFileName;
    realFileName.Format("@user@/cache/%s", filename);

    const uint32 fileOpenFlags = ICryPak::FLAGS_NEVER_IN_PAK | ICryPak::FLAGS_PATH_REAL | ICryPak::FOPEN_ONDISK;
    AZ::IO::HandleType fileHandle = gEnv->pCryPak->FOpen(realFileName.c_str(), "rbx", fileOpenFlags);
    if (fileHandle == AZ::IO::InvalidHandle)
    {
        CryLog("DoesCachePakExist '%s' ERROR file not found '%s'", filename, realFileName.c_str());
        return IPlatformOS::eCDPO_FileNotFound;
    }
    gEnv->pCryPak->FSeek(fileHandle, 0, SEEK_END);
    const size_t fileSize = (size_t)(gEnv->pCryPak->FTell(fileHandle));
    gEnv->pCryPak->FClose(fileHandle);

    if (fileSize != size)
    {
        CryLog("DoesCachePakExist '%s' ERROR size doesn't match Cache:%" PRISIZE_T " Input:%" PRISIZE_T " '%s'", filename, fileSize, size, realFileName.c_str());
        return IPlatformOS::eCDPO_SizeNotMatch;
    }

    ret = gEnv->pCryPak->ComputeMD5(realFileName.c_str(), fileMD5, fileOpenFlags);
    if (ret == false)
    {
        CryLog("DoesCachePakExist '%s' ERROR during ComputeMD5 '%s'", filename, realFileName.c_str());
        return IPlatformOS::eCDPO_MD5Failed;
    }
    if (memcmp(fileMD5, md5, 16) != 0)
    {
        CryLog("DoesCachePakExist '%s' ERROR hashes don't match '%s'", filename, realFileName.c_str());
        CryLog("Computed MD5 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            fileMD5[0], fileMD5[1], fileMD5[2], fileMD5[3],
            fileMD5[4], fileMD5[5], fileMD5[6], fileMD5[7],
            fileMD5[8], fileMD5[9], fileMD5[10], fileMD5[11],
            fileMD5[12], fileMD5[13], fileMD5[14], fileMD5[15]);
        CryLog("Manifest MD5 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            md5[0], md5[1], md5[2], md5[3],
            md5[4], md5[5], md5[6], md5[7],
            md5[8], md5[9], md5[10], md5[11],
            md5[12], md5[13], md5[14], md5[15]);

        return IPlatformOS::eCDPO_HashNotMatch;
    }

    return IPlatformOS::eCDPO_Success;
}


IPlatformOS::ECDP_Open CPlatformOS_PC::OpenCachePak(const char* const filename, const char* const bindRoot, const size_t size, unsigned char md5[16])
{
    bool ret;
    IPlatformOS::ECDP_Open existsResult = DoesCachePakExist(filename, size, md5);
    if (existsResult != IPlatformOS::eCDPO_Success)
    {
        return existsResult;
    }

    CryFixedStringT<128> realFileName;
    realFileName.Format("@user@/cache/%s", filename);

    const uint32 fileOpenFlags = ICryPak::FLAGS_NEVER_IN_PAK | ICryPak::FLAGS_PATH_REAL | ICryPak::FOPEN_ONDISK;
    const uint32 pakOpenFlags = fileOpenFlags | ICryArchive::FLAGS_OVERRIDE_PAK;
    ret = gEnv->pCryPak->OpenPack(bindRoot, realFileName.c_str(), pakOpenFlags);
    if (ret == false)
    {
        CryLog("OpenCachePak '%s' ERROR during OpenPack '%s'", filename, realFileName.c_str());
        return IPlatformOS::eCDPO_OpenPakFailed;
    }

    return IPlatformOS::eCDPO_Success;
}

IPlatformOS::ECDP_Close CPlatformOS_PC::CloseCachePak(const char* const filename)
{
    if (m_cachePakStatus != 1)
    {
        CryLog("CloseCachePak '%s' ERROR cache paks not ready to be used", filename);
        return IPlatformOS::eCDPC_NotInUse;
    }

    CryFixedStringT<128> realFileName;
    realFileName.Format("@user@/cache/%s", filename);
    const uint32 pakOpenFlags = ICryPak::FLAGS_NEVER_IN_PAK | ICryPak::FLAGS_PATH_REAL | ICryPak::FOPEN_ONDISK | ICryArchive::FLAGS_OVERRIDE_PAK;

    const bool ret = gEnv->pCryPak->ClosePack(realFileName.c_str(), pakOpenFlags);
    if (ret == false)
    {
        CryLog("CloseCachePak '%s' ERROR during ClosePack '%s'", filename, realFileName.c_str());
        return IPlatformOS::eCDPC_Failure;
    }
    return IPlatformOS::eCDPC_Success;
}

IPlatformOS::ECDP_Delete CPlatformOS_PC::DeleteCachePak(const char* const filename)
{
    if (m_cachePakStatus != 1)
    {
        CryLog("DeleteCachePak '%s' ERROR cache paks not ready to be used", filename);
        return IPlatformOS::eCDPD_NotInUse;
    }

    CryFixedStringT<128> realFileName;
    realFileName.Format("@user@/cache/%s", filename);
    const bool ret = gEnv->pCryPak->RemoveFile(realFileName.c_str());
    if (ret == false)
    {
        CryLog("DeleteCachePak '%s' ERROR during RemoveFile '%s'", filename, realFileName.c_str());
        return IPlatformOS::eCDPD_Failure;
    }
    return IPlatformOS::eCDPD_Success;
}

IPlatformOS::ECDP_Write CPlatformOS_PC::WriteCachePak(const char* const filename, const void* const pData, const size_t numBytes)
{
    if (m_cachePakStatus != 1)
    {
        CryLog("WriteCachePak '%s' ERROR cache paks not ready to be used", filename);
        return IPlatformOS::eCDPW_NotInUse;
    }

    CryFixedStringT<128> realFileName;
    realFileName.Format("@user@/cache/%s", filename);
    const uint32 nWriteFlags = ICryPak::FLAGS_PATH_REAL | ICryPak::FOPEN_ONDISK;
    AZ::IO::HandleType fileHandle = gEnv->pCryPak->FOpen(realFileName.c_str(), "wb", nWriteFlags);
    if (fileHandle == AZ::IO::InvalidHandle)
    {
        CryLog("WriteCachePak '%s' ERROR FOpen '%s'", filename, realFileName.c_str());
        return IPlatformOS::eCDPW_NoFreeSpace;
    }
    size_t numBytesWritten = gEnv->pCryPak->FWrite(pData, 1, numBytes, fileHandle);
    if (numBytesWritten != numBytes)
    {
        CryLog("WriteCachePak '%s' ERROR FWrite failed %" PRISIZE_T " written size %" PRISIZE_T " '%s'", filename, numBytesWritten, numBytes, realFileName.c_str());
        return IPlatformOS::eCDPW_Failure;
    }
    const int ret = gEnv->pCryPak->FClose(fileHandle);
    if (ret != 0)
    {
        CryLog("WriteCachePak '%s' ERROR FClose failed %d '%s'", filename, ret, realFileName.c_str());
        return IPlatformOS::eCDPW_Failure;
    }
    return IPlatformOS::eCDPW_Success;
}

IPlatformOS::IFileFinderPtr CPlatformOS_PC::GetFileFinder(unsigned int user)
{
#if USE_STEAM
    if (UseSteamReadWriter())
    {
        return IFileFinderPtr(new CFileFinderSteam());
    }
#endif

    return IFileFinderPtr(new CFileFinderCryPak());
}

#endif
