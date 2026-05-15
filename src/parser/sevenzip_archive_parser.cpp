#include "sevenzip_archive_parser.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../string_utils.h"

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "7zTypes.h"
}

namespace EveryZip {

static constexpr size_t kInputBufSize = 1u << 18;
static constexpr UInt32 kNoFolder = 0xFFFFFFFFu;

static const ISzAlloc kAlloc = { SzAlloc, SzFree };
static const ISzAlloc kAllocTemp = { SzAllocTemp, SzFreeTemp };

struct SevenZipArchiveParser::State {
    CFileInStream archiveStream{};
    CLookToRead2 lookStream{};
    CSzArEx db{};
    std::wstring archivePath;
    bool fileOpen = false;
    bool dbOpen = false;
    bool lookBufferAllocated = false;
};

static void EnsureCrcTableReady() {
    static std::once_flag once;
    std::call_once(once, []() {
        CrcGenerateTable();
    });
}

static std::string SzErrorToString(SRes res) {
    switch (res) {
    case SZ_OK: return "ok";
    case SZ_ERROR_DATA: return "data error";
    case SZ_ERROR_MEM: return "out of memory";
    case SZ_ERROR_CRC: return "crc error";
    case SZ_ERROR_UNSUPPORTED: return "unsupported method";
    case SZ_ERROR_PARAM: return "invalid parameter";
    case SZ_ERROR_INPUT_EOF: return "unexpected input eof";
    case SZ_ERROR_OUTPUT_EOF: return "unexpected output eof";
    case SZ_ERROR_READ: return "read error";
    case SZ_ERROR_WRITE: return "write error";
    case SZ_ERROR_PROGRESS: return "progress error";
    case SZ_ERROR_FAIL: return "operation failed";
    case SZ_ERROR_THREAD: return "thread error";
    case SZ_ERROR_ARCHIVE: return "archive error";
    case SZ_ERROR_NO_ARCHIVE: return "not a 7z archive";
    default: return "7z error: " + std::to_string(res);
    }
}

static bool GetItemName(const CSzArEx& db, UInt32 index, std::wstring* out, std::string* error) {
    const size_t len = SzArEx_GetFileNameUtf16(&db, index, nullptr);
    if (len == 0) {
        if (error) *error = "SzArEx_GetFileNameUtf16 failed";
        return false;
    }

    std::vector<UInt16> name(len);
    const size_t written = SzArEx_GetFileNameUtf16(&db, index, name.data());
    if (written == 0) {
        if (error) *error = "SzArEx_GetFileNameUtf16 failed";
        return false;
    }

    if (out) {
        *out = Utf16UnitsToWString(name.data(), written);
    }
    return true;
}

static UInt32 GetItemFolderIndex(const CSzArEx& db, UInt32 index) {
    if (!db.FileToFolder || index >= db.NumFiles) return kNoFolder;
    return db.FileToFolder[index];
}

static std::int64_t GetPackSizeFallback(const CSzArEx& db, UInt32 index) {
    const UInt32 folderIndex = GetItemFolderIndex(db, index);
    if (folderIndex == kNoFolder || folderIndex >= db.db.NumFolders) return -1;

    if (!db.db.FoStartPackStreamIndex || !db.db.PackPositions) return -1;
    const UInt32 startPackStream = db.db.FoStartPackStreamIndex[folderIndex];
    const UInt32 endPackStream = db.db.FoStartPackStreamIndex[folderIndex + 1];
    if (startPackStream > endPackStream || endPackStream > db.db.NumPackStreams) return -1;
    return (std::int64_t)(db.db.PackPositions[endPackStream] - db.db.PackPositions[startPackStream]);
}

static std::vector<UInt32> CountFilesPerFolder(const CSzArEx& db) {
    std::vector<UInt32> counts(db.db.NumFolders, 0);
    for (UInt32 i = 0; i < db.NumFiles; ++i) {
        if (SzArEx_IsDir(&db, i)) continue;

        const UInt32 folderIndex = GetItemFolderIndex(db, i);
        if (folderIndex != kNoFolder && folderIndex < counts.size()) {
            ++counts[folderIndex];
        }
    }
    return counts;
}

SevenZipArchiveParser::SevenZipArchiveParser()
    : state_(std::make_unique<State>())
{
    File_Construct(&state_->archiveStream.file);
    SzArEx_Init(&state_->db);
}

SevenZipArchiveParser::~SevenZipArchiveParser() {
    Close();
}

bool SevenZipArchiveParser::Open(const std::wstring& archive_path, std::string* error) {
    Close();

    if (archive_path.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }

    EnsureCrcTableReady();

    File_Construct(&state_->archiveStream.file);
    WRes wres = InFile_OpenW(&state_->archiveStream.file, archive_path.c_str());
    if (wres != 0) {
        if (error) *error = "InFile_OpenW failed: " + std::to_string((unsigned)wres);
        return false;
    }
    state_->fileOpen = true;

    FileInStream_CreateVTable(&state_->archiveStream);
    state_->archiveStream.wres = 0;

    LookToRead2_CreateVTable(&state_->lookStream, False);
    state_->lookStream.buf = (Byte*)ISzAlloc_Alloc(&kAlloc, kInputBufSize);
    if (!state_->lookStream.buf) {
        if (error) *error = "failed to allocate 7z input buffer";
        Close();
        return false;
    }
    state_->lookBufferAllocated = true;
    state_->lookStream.bufSize = kInputBufSize;
    state_->lookStream.realStream = &state_->archiveStream.vt;
    LookToRead2_INIT(&state_->lookStream);

    SzArEx_Init(&state_->db);
    const SRes res = SzArEx_Open(&state_->db, &state_->lookStream.vt, &kAlloc, &kAllocTemp);
    if (res != SZ_OK) {
        if (error) *error = "SzArEx_Open failed: " + SzErrorToString(res);
        Close();
        return false;
    }

    state_->dbOpen = true;
    state_->archivePath = archive_path;
    return true;
}

void SevenZipArchiveParser::Close() {
    if (!state_) return;

    if (state_->dbOpen) {
        SzArEx_Free(&state_->db, &kAlloc);
        state_->dbOpen = false;
    }

    if (state_->lookBufferAllocated && state_->lookStream.buf) {
        ISzAlloc_Free(&kAlloc, state_->lookStream.buf);
        state_->lookStream.buf = nullptr;
        state_->lookBufferAllocated = false;
    }

    if (state_->fileOpen) {
        File_Close(&state_->archiveStream.file);
        state_->fileOpen = false;
    }

    state_->archivePath.clear();
    SzArEx_Init(&state_->db);
}

bool SevenZipArchiveParser::IsOpen() const {
    return state_ && state_->dbOpen;
}

std::wstring SevenZipArchiveParser::ArchivePath() const {
    return state_ ? state_->archivePath : std::wstring();
}

bool SevenZipArchiveParser::ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) {
    if (!out_entries) {
        if (error) *error = "out_entries is null";
        return false;
    }
    out_entries->clear();

    if (!IsOpen()) {
        if (error) *error = "archive is not open";
        return false;
    }

    const std::vector<UInt32> filesPerFolder = CountFilesPerFolder(state_->db);
    out_entries->reserve(state_->db.NumFiles);
    for (UInt32 i = 0; i < state_->db.NumFiles; ++i) {
        std::wstring nameW;
        if (!GetItemName(state_->db, i, &nameW, error)) {
            return false;
        }

        ArchiveEntry_t e;
        e.entryPathUtf8 = WideToUtf8(nameW);
        e.isDirectory = SzArEx_IsDir(&state_->db, i) != 0;
        e.originalSize = e.isDirectory ? 0 : (std::uint64_t)SzArEx_GetFileSize(&state_->db, i);
        e.compressedSize = 0;
        if (!e.isDirectory) {
            const UInt32 folderIndex = GetItemFolderIndex(state_->db, i);
            if (folderIndex != kNoFolder &&
                folderIndex < filesPerFolder.size() &&
                filesPerFolder[folderIndex] == 1) {
                e.compressedSize = GetPackSizeFallback(state_->db, i);
            } else {
                e.compressedSize = -1;
            }
        }

        if (SzBitWithVals_Check(&state_->db.MTime, i)) {
            const CNtfsFileTime& t = state_->db.MTime.Vals[i];
            FILETIME ft{};
            ft.dwLowDateTime = t.Low;
            ft.dwHighDateTime = t.High;
            FILETIME localFt{};
            SYSTEMTIME st{};
            if (FileTimeToLocalFileTime(&ft, &localFt) && FileTimeToSystemTime(&localFt, &st)) {
                std::tm localTime{};
                localTime.tm_sec = st.wSecond;
                localTime.tm_min = st.wMinute;
                localTime.tm_hour = st.wHour;
                localTime.tm_mday = st.wDay;
                localTime.tm_mon = st.wMonth - 1;
                localTime.tm_year = st.wYear - 1900;
                e.modifiedTime = LocalTmToFileTimeValue(localTime);
            }
        }

        out_entries->push_back(std::move(e));
    }

    return true;
}

bool SevenZipArchiveParser::ExtractEntry(const std::string& entry_path,
                                         const std::wstring& dest_dir,
                                         std::string* error) {
    if (!IsOpen()) {
        if (error) *error = "archive is not open";
        return false;
    }
    if (entry_path.empty()) {
        if (error) *error = "entry_path is empty";
        return false;
    }

    UInt32 targetIndex = kNoFolder;
    for (UInt32 i = 0; i < state_->db.NumFiles; ++i) {
        std::wstring nameW;
        if (!GetItemName(state_->db, i, &nameW, error)) {
            return false;
        }
        std::string nameUtf8 = WideToUtf8(nameW);
        std::replace(nameUtf8.begin(), nameUtf8.end(), '\\', '/');
        std::string target = entry_path;
        std::replace(target.begin(), target.end(), '\\', '/');
        if (nameUtf8 == target) {
            targetIndex = i;
            break;
        }
    }

    if (targetIndex == kNoFolder) {
        if (error) *error = "entry not found: " + entry_path;
        return false;
    }

    if (SzArEx_IsDir(&state_->db, targetIndex)) {
        std::wstring dirPath = dest_dir;
        if (!dirPath.empty() && dirPath.back() != L'\\') dirPath += L'\\';
        dirPath += GetEntryNameFromPath(Utf8ToWString(entry_path.c_str()));
        if (!dirPath.empty()) {
            CreateDirectoryW(dirPath.c_str(), nullptr);
        }
        return true;
    }

    UInt32 blockIndex = kNoFolder;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    size_t offset = 0;
    size_t outSizeProcessed = 0;

    const SRes res = SzArEx_Extract(&state_->db, &state_->lookStream.vt, targetIndex,
                                    &blockIndex, &outBuffer, &outBufferSize,
                                    &offset, &outSizeProcessed, &kAlloc, &kAllocTemp);
    if (res != SZ_OK) {
        ISzAlloc_Free(&kAlloc, outBuffer);
        if (error) *error = "SzArEx_Extract failed: " + SzErrorToString(res);
        return false;
    }

    std::wstring entryName = GetEntryNameFromPath(Utf8ToWString(entry_path.c_str()));
    if (entryName.empty()) {
        ISzAlloc_Free(&kAlloc, outBuffer);
        if (error) *error = "entry has no filename: " + entry_path;
        return false;
    }

    std::wstring destPath = dest_dir;
    if (!destPath.empty() && destPath.back() != L'\\') destPath += L'\\';
    destPath += entryName;

    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        ISzAlloc_Free(&kAlloc, outBuffer);
        if (error) *error = "CreateFileW failed for: " + entry_path;
        return false;
    }

    bool ok = true;
    size_t remaining = outSizeProcessed;
    const Byte* writePtr = outBuffer + offset;
    while (remaining > 0) {
        const DWORD chunk = (DWORD)std::min<size_t>(remaining, 1u << 20);
        DWORD written = 0;
        if (!WriteFile(hFile, writePtr, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        writePtr += written;
        remaining -= written;
    }
    CloseHandle(hFile);
    ISzAlloc_Free(&kAlloc, outBuffer);

    if (!ok) {
        if (error) *error = "WriteFile failed";
        return false;
    }
    return true;
}

} // namespace EveryZip
