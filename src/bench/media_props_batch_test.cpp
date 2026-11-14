#include "../app/media_props.h"
#include <objbase.h>
#include <cstdio>
#include <chrono>
#include <thread>

using namespace pulse::app::media;

bool WaitIdle() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Pending() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return Pending() == 0;
}

// A 3x2 24-bit BMP: real enough for the shell property handler to open.
bool WriteBmp(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + 24;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = 3;
    info.biHeight = 2;
    info.biPlanes = 1;
    info.biBitCount = 24;
    BYTE pixels[24]{};
    DWORD bytes = 0;
    bool written = WriteFile(file, &header, sizeof(header), &bytes, nullptr) && bytes == sizeof(header);
    written &= WriteFile(file, &info, sizeof(info), &bytes, nullptr) && bytes == sizeof(info);
    written &= WriteFile(file, pixels, sizeof(pixels), &bytes, nullptr) && bytes == sizeof(pixels);
    CloseHandle(file);
    return written;
}

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const std::wstring image_path = std::wstring(cwd) + L"\\bench_data\\media-props-" +
        std::to_wstring(GetCurrentProcessId()) + L".bmp";
    const bool written = WriteBmp(image_path);
    check(written, "create isolated BMP fixture");
    if (written) {
        Job image_job;
        image_job.path = L"\\\\?\\" + image_path;
        image_job.kind = Kind::Photo;
        const auto start = std::chrono::steady_clock::now();
        const Props props = ReadProps(image_job);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        check(props.width == 3 && props.height == 2, "real BMP dimensions through selected property keys and long-path prefix");
        std::printf("BMP property read: %lld ms\n", static_cast<long long>(elapsed));
    }
    if (written) check(DeleteFileW(image_path.c_str()) != FALSE, "remove BMP fixture");
    CoUninitialize();
    int owner = 0;
    std::vector<Job> jobs;
    for (unsigned i = 0; i < 96; ++i) {
        Job job;
        job.path = L"\\\\?\\C:\\pulse-media-props-test\\" + std::to_wstring(i);
        job.mtime = 1;
        job.size = i;
        job.attrs = FILE_ATTRIBUTE_OFFLINE;
        job.kind = Kind::Photo;
        jobs.push_back(job);
        jobs.push_back(job); // Multiple tabs can request identical versions.
    }
    const auto before = Revision();
    Request(&owner, jobs);
    check(WaitIdle(), "batch completes, including final partial batch");
    const auto updates = Revision() - before;
    check(updates >= 3 && updates < 96, "revision published per batch rather than per file");
    bool all_cached = true;
    for (const auto& job : jobs) {
        const auto props = Find(job.path, job.mtime, job.size);
        all_cached &= props && props->width == 0 && props->taken == 0;
    }
    check(all_cached, "offline metadata cached without opening files");
    Request(&owner, jobs);
    check(WaitIdle() && Revision() == before + updates, "current versions cause no new publications");
    Job changed = jobs.front();
    changed.mtime = 2;
    Request(&owner, {changed});
    check(WaitIdle() && Find(changed.path, 2, changed.size) && !Find(changed.path, 1, changed.size),
          "single-file tail and changed file version published");
    size_t count = 0;
    ForEachInFolder(L"\\\\?\\C:\\pulse-media-props-test\\", [&](auto, auto, auto, const auto&) { ++count; });
    check(count == 96, "folder cache contains every unique file");
    std::printf("publications for 192 requests / 96 unique files: %llu\n",
                static_cast<unsigned long long>(updates));

    // Rating read-backs: the writer re-reads each file it rated under one
    // owner, the next one usually while the previous read is still running.
    const std::wstring slow_path = std::wstring(cwd) + L"\\bench_data\\media-props-refresh-" +
        std::to_wstring(GetCurrentProcessId()) + L".bmp";
    const bool slow_written = WriteBmp(slow_path);
    check(slow_written, "create isolated refresh BMP fixture");
    if (slow_written) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        GetFileAttributesExW(slow_path.c_str(), GetFileExInfoStandard, &data);
        Job slow;  // a real read: the property handler opens the file
        slow.path = L"\\\\?\\" + slow_path;
        slow.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                     data.ftLastWriteTime.dwLowDateTime;
        slow.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        slow.attrs = data.dwFileAttributes;
        slow.kind = Kind::Photo;
        Job quick;
        quick.path = L"\\\\?\\C:\\pulse-media-props-test\\refresh";
        quick.mtime = 1;
        quick.attrs = FILE_ATTRIBUTE_OFFLINE;
        quick.kind = Kind::Photo;
        int writer = 0;
        Refresh(&writer, {slow});
        Refresh(&writer, {quick});
        check(WaitIdle(), "back-to-back refreshes finish");
        const auto slow_props = Find(slow.path, slow.mtime, slow.size);
        check(slow_props && slow_props->width == 3,
              "a refresh still being read survives the owner's next refresh");
        check(Find(quick.path, quick.mtime, quick.size) != nullptr, "the owner's next refresh publishes");
        check(DeleteFileW(slow_path.c_str()) != FALSE, "remove refresh BMP fixture");
    }
    return failed ? 1 : 0;
}
