// SPDX-License-Identifier: GPL-3.0-or-later
// fsp-srv (S3c): the file system service of the application.
//   * RomFS: OpenDataStorageByCurrentProcess returns an IStorage over a RomFS image synthesised from the loose game folder
//     ([fs] romfs, default game/romfs; fs_romfs.h). The SDK parses the RomFS itself (nn::fs::MountRom), so the storage is byte addressed.
//     OpenDataFileSystemByCurrentProcess serves the same folder as a read-only IFileSystem.
//   * Save data: account, device and cache saves live in folders under [fs] save_dir (default %APPDATA%\<game save_folder>\save, nxrt/game.h; MPRRecomp for Metroid Prime Remastered) with the journal
//     semantics of fs_volume.h: writes are invisible to the committed state until IFileSystem::Commit.
//   * Mods: the RomFS mods under [fs] mods (installed_mods(), services_internal.h) are laid over that image (LayeredFS style).
//   * Add-on content: OpenDataStorageByDataId serves the RomFS of the DLC the player supplied from their own dump ([aoc] dir, default
//     game/dlc; installed_aoc() in services_internal.h).
//   * SD card, game card, BIS: absent (the results of a console without them).
// Command ids, argument layouts: Switchbrew "Filesystem services", ScarletNX fs.swipc (GPL-3.0-or-later), Ryujinx IFileSystemProxy (MIT).
// The commit/recovery design is smo-recomp runtime/fs/nnfs.c (GPL-3.0), see fs_volume.h.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "fs_romfs.h"
#include "nxrt/nup.h"
#include "fs_volume.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace stdfs = std::filesystem;

namespace nxrt::services {

namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;
using fs::Volume;

constexpr size_t kPathBuf = 0x301;
constexpr uint32_t kUnsupportedWrite = fs::rc::kReadOnlyFileSystem;

// ---- process-wide state ---------------------------------------------------------------------------------------------------------

struct FsState {
    std::mutex mu;
    std::shared_ptr<fs::RomfsImage> rom;
    bool rom_failed = false;
    std::map<uint64_t, std::shared_ptr<fs::RomfsImage>> aoc_rom;   // add-on content data id -> its RomFS image
    std::map<std::string, std::weak_ptr<Volume>> saves;   // key -> open volume (one at a time per save)
    std::map<uint64_t, std::string> ids;                  // SaveDataId -> key, for the commands that name a save by id
};
FsState& st() {
    static FsState s;
    return s;
}

// ---- save data keys ---------------------------------------------------------------------------------------------------------------

struct SaveAttr {
    uint64_t application_id = 0;
    uint8_t uid[16] = {};
    uint64_t system_id = 0;
    uint8_t type = 0, rank = 0;
    uint16_t index = 0;
    uint8_t raw[0x40] = {};
};

SaveAttr parse_attr(const uint8_t* p) {
    SaveAttr a;
    std::memcpy(a.raw, p, 0x40);
    std::memcpy(&a.application_id, p, 8);
    std::memcpy(a.uid, p + 8, 16);
    std::memcpy(&a.system_id, p + 24, 8);
    a.type = p[32];
    a.rank = p[33];
    std::memcpy(&a.index, p + 34, 2);
    if (a.application_id == 0) a.application_id = profile().program_id;
    return a;
}

std::string hex_of(const uint8_t* p, size_t n) {
    char b[3];
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// The folder name (relative, '/' separated) of a save: SaveDataType 0 System, 1 Account, 2 Bcat, 3 Device, 4 Temporary, 5 Cache, 6 SystemBcat.
std::string save_key(const SaveAttr& a) {
    std::string k;
    if (a.application_id != profile().program_id) {
        char b[24];
        std::snprintf(b, sizeof b, "app%016llx/", static_cast<unsigned long long>(a.application_id));
        k = b;
    }
    char b[40];
    switch (a.type) {
        case 1: k += "account/" + hex_of(a.uid, 16); break;
        case 3: k += "device"; break;
        case 5: std::snprintf(b, sizeof b, "cache/%u", a.index); k += b; break;
        case 0:
        case 6: std::snprintf(b, sizeof b, "system/%016llx", static_cast<unsigned long long>(a.system_id)); k += b; break;
        case 2: k += "bcat"; break;
        default: std::snprintf(b, sizeof b, "temporary/%u", a.index); k += b; break;
    }
    return k;
}

uint64_t save_id_of(const std::string& key) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
    return h;
}

stdfs::path root_of(const std::string& key) {
    stdfs::path p = stdfs::u8path(save_dir());
    p /= stdfs::u8path(key);
    return p;
}
stdfs::path meta_of(const std::string& key) { return stdfs::path(root_of(key).string() + ".meta"); }

struct SaveMeta {
    int64_t size = 0, journal = 0;
    uint64_t owner = 0;
    std::string attr_hex;
    bool ok = false;
};
SaveMeta read_meta(const std::string& key) {
    SaveMeta m;
    std::ifstream in(meta_of(key));
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "size") m.size = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "journal") m.journal = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "owner") m.owner = std::strtoull(v.c_str(), nullptr, 16);
        else if (k == "attr") m.attr_hex = v;
        m.ok = true;
    }
    return m;
}
void write_meta(const std::string& key, const SaveMeta& m) {
    std::error_code ec;
    stdfs::create_directories(meta_of(key).parent_path(), ec);
    const stdfs::path tmp(meta_of(key).string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "size=" << m.size << "\njournal=" << m.journal << "\nowner=" << std::hex << m.owner << std::dec << "\nattr=" << m.attr_hex << "\n";
    }
    stdfs::rename(tmp, meta_of(key), ec);
}

bool save_exists(const std::string& key) {
    std::error_code ec;
    return stdfs::exists(meta_of(key), ec) && stdfs::is_directory(root_of(key), ec);
}

uint32_t create_save(const SaveAttr& a, int64_t size, int64_t journal, uint64_t owner) {
    const std::string key = save_key(a);
    if (save_exists(key)) return fs::rc::kPathAlreadyExists;
    std::error_code ec;
    stdfs::create_directories(root_of(key), ec);
    if (ec) return fs::rc::kGenericFailure;
    SaveMeta m;
    m.size = size;
    m.journal = journal;
    m.owner = owner ? owner : profile().save_data_owner_id;
    m.attr_hex = hex_of(a.raw, 0x40);
    write_meta(key, m);
    std::lock_guard<std::mutex> lk(st().mu);
    st().ids[save_id_of(key)] = key;
    NXLOG_I("fs", "save data created: %s (%lld bytes, journal %lld)", key.c_str(), static_cast<long long>(size), static_cast<long long>(journal));
    return 0;
}

std::shared_ptr<Volume> open_save_volume(const std::string& key, bool read_only) {
    std::lock_guard<std::mutex> lk(st().mu);
    auto it = st().saves.find(key);
    if (!read_only && it != st().saves.end())
        if (auto v = it->second.lock()) return v;
    const SaveMeta m = read_meta(key);
    auto v = std::make_shared<Volume>(root_of(key).string(), true, read_only, static_cast<uint64_t>(m.size));
    if (!v->ok()) return nullptr;
    if (!read_only) st().saves[key] = v;
    st().ids[save_id_of(key)] = key;
    return v;
}

// ---- the interfaces -----------------------------------------------------------------------------------------------------------

bool get_path(const Request& req, size_t n, std::string* out, uint32_t* err) {
    const ipc::Buffer b = req.in_buf(n);
    if (!b.addr) {
        *err = fs::rc::kInvalidArgument;
        return false;
    }
    return fs::normalize_path(reinterpret_cast<const char*>(static_cast<uintptr_t>(b.addr)), static_cast<size_t>(std::min<uint64_t>(b.size, kPathBuf)), out, err);
}

#define PATH_OR_FAIL(var, n)                       \
    std::string var;                               \
    {                                              \
        uint32_t perr = 0;                         \
        if (!get_path(req, n, &var, &perr)) return perr; \
    }

// Gauges (include/nxrt/gauge.h). An open file or directory counts while its service object lives: the command handlers hold OpenCount.
GaugeCounter g_open_files("fs.open_files");
GaugeCounter g_open_dirs("fs.open_dirs");
GaugeCounter g_bytes_read("fs.bytes_read");   // bytes returned by IFile::Read so far
struct OpenCount {
    explicit OpenCount(GaugeCounter& g) : c(g) { c.add(1); }
    ~OpenCount() { c.sub(1); }
    OpenCount(const OpenCount&) = delete;
    OpenCount& operator=(const OpenCount&) = delete;
    GaugeCounter& c;
};

std::shared_ptr<Service> make_file(std::shared_ptr<fs::VFile> f, const std::string& path) {
    auto open = std::make_shared<OpenCount>(g_open_files);
    return make_iface("nn::fssrv::sf::IFile", [f, path, open](Service& s) {
        s.add_cmd(0, "Read", [f, open](Request& req, Response& rsp) -> uint32_t {
            const int64_t off = req.arg<int64_t>(8);
            const uint64_t size = req.arg<uint64_t>(16);
            const ipc::Buffer out = req.out_buf(0);
            if (!out.addr && size) return fs::rc::kInvalidArgument;
            uint64_t got = 0;
            const uint32_t r = f->read(off, reinterpret_cast<void*>(static_cast<uintptr_t>(out.addr)), std::min(size, out.size), &got);
            if (r) return r;
            g_bytes_read.add(static_cast<int64_t>(got));
            rsp.push<uint64_t>(got);
            return 0;
        });
        s.add_cmd(1, "Write", [f](Request& req, Response&) -> uint32_t {
            const int64_t off = req.arg<int64_t>(8);
            const uint64_t size = req.arg<uint64_t>(16);
            const ipc::Buffer in = req.in_buf(0);
            if (!in.addr && size) return fs::rc::kInvalidArgument;
            if (in.size < size) return fs::rc::kInvalidArgument;
            const uint32_t r = f->write(off, reinterpret_cast<const void*>(static_cast<uintptr_t>(in.addr)), size);
            if (r) return r;
            if (req.arg<uint32_t>(0) & 1) return f->flush();   // WriteOption flush
            return 0;
        });
        s.add_cmd(2, "Flush", [f](Request&, Response&) -> uint32_t { return f->flush(); });
        s.add_cmd(3, "SetSize", [f](Request& req, Response&) -> uint32_t { return f->set_size(req.arg<int64_t>(0)); });
        s.add_cmd(4, "GetSize", [f](Request&, Response& rsp) -> uint32_t {
            int64_t sz = 0;
            const uint32_t r = f->get_size(&sz);
            if (r) return r;
            rsp.push<int64_t>(sz);
            return 0;
        });
        s.add_cmd(5, "OperateRange", [](Request&, Response& rsp) -> uint32_t {   // QueryRangeInfo (0x40 bytes), nothing to report
            rsp.out.insert(rsp.out.end(), 0x40, uint8_t{0});
            return 0;
        });
    });
}

std::shared_ptr<Service> make_dir(std::vector<fs::DirItem> items) {
    auto list = std::make_shared<std::vector<fs::DirItem>>(std::move(items));
    auto pos = std::make_shared<size_t>(0);
    auto open = std::make_shared<OpenCount>(g_open_dirs);
    return make_iface("nn::fssrv::sf::IDirectory", [list, pos, open](Service& s) {
        s.add_cmd(0, "Read", [list, pos, open](Request& req, Response& rsp) -> uint32_t {
            const ipc::Buffer out = req.out_buf(0);
            const size_t cap = static_cast<size_t>(out.size / 0x310);
            size_t n = 0;
            while (n < cap && *pos < list->size()) {
                uint8_t* e = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out.addr)) + n * 0x310;
                std::memset(e, 0, 0x310);
                const fs::DirItem& it = (*list)[*pos];
                std::memcpy(e, it.name.data(), std::min<size_t>(it.name.size(), 0x300));
                e[0x304] = it.is_dir ? 0 : 1;
                const int64_t sz = it.is_dir ? 0 : it.size;
                std::memcpy(e + 0x308, &sz, 8);
                n++;
                (*pos)++;
            }
            rsp.push<int64_t>(static_cast<int64_t>(n));
            return 0;
        });
        s.add_cmd(1, "GetEntryCount", [list](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(static_cast<int64_t>(list->size()));
            return 0;
        });
    });
}

std::shared_ptr<Service> make_filesystem(std::shared_ptr<Volume> v, std::string label) {
    return make_iface("nn::fssrv::sf::IFileSystem", [v, label](Service& s) {
        s.add_cmd(0, "CreateFile", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->create_file(p, req.arg<int64_t>(8));
        });
        s.add_cmd(1, "DeleteFile", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->delete_file(p);
        });
        s.add_cmd(2, "CreateDirectory", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->create_dir(p);
        });
        s.add_cmd(3, "DeleteDirectory", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->delete_dir(p);
        });
        s.add_cmd(4, "DeleteDirectoryRecursively", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->delete_dir_recursive(p);
        });
        s.add_cmd(5, "RenameFile", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(a, 0);
            PATH_OR_FAIL(b, 1);
            return v->rename_file(a, b);
        });
        s.add_cmd(6, "RenameDirectory", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(a, 0);
            PATH_OR_FAIL(b, 1);
            return v->rename_dir(a, b);
        });
        s.add_cmd(7, "GetEntryType", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            uint32_t t = 0;
            const uint32_t r = v->entry_type(p, &t);
            if (r) return r;
            rsp.push<uint32_t>(t);
            return 0;
        });
        s.add_cmd(8, "OpenFile", [v, label](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            std::shared_ptr<fs::VFile> f;
            const uint32_t r = v->open_file(p, req.arg<uint32_t>(0), &f);
            diag_note_file("open", (label + p).c_str());
            if (r) {
                NXLOG_D("fs", "OpenFile %s%s (mode %u) -> 0x%x", label.c_str(), p.c_str(), req.arg<uint32_t>(0), r);
                return r;
            }
            rsp.push_object(make_file(f, p));
            return 0;
        });
        s.add_cmd(9, "OpenDirectory", [v, label](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            std::vector<fs::DirItem> items;
            const uint32_t r = v->list_dir(p, &items);
            diag_note_file("dir", (label + p).c_str());
            if (r) return r;
            const uint32_t mode = req.arg<uint32_t>(0);   // 1 directories, 2 files
            std::vector<fs::DirItem> keep;
            for (const fs::DirItem& it : items)
                if ((it.is_dir && (mode & 1)) || (!it.is_dir && (mode & 2))) keep.push_back(it);
            rsp.push_object(make_dir(std::move(keep)));
            return 0;
        });
        s.add_cmd(10, "Commit", [v](Request&, Response&) -> uint32_t { return v->commit(); });
        s.add_cmd(11, "GetFreeSpaceSize", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            const uint64_t cap = v->capacity();
            const uint64_t used = v->used_bytes();
            rsp.push<int64_t>(cap ? static_cast<int64_t>(cap > used ? cap - used : 0) : (int64_t{1} << 40));
            return 0;
        });
        s.add_cmd(12, "GetTotalSpaceSize", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            rsp.push<int64_t>(v->capacity() ? static_cast<int64_t>(v->capacity()) : (int64_t{1} << 40));
            return 0;
        });
        s.add_cmd(13, "CleanDirectoryRecursively", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->clean_dir_recursive(p);
        });
        s.add_cmd(14, "GetFileTimeStampRaw", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            fs::FileTimes t;
            const uint32_t r = v->file_times(p, &t);
            if (r) return r;
            rsp.push<int64_t>(t.created);
            rsp.push<int64_t>(t.modified);
            rsp.push<int64_t>(t.accessed);
            rsp.push<uint64_t>(t.valid ? 1 : 0);   // is_valid + padding
            return 0;
        });
        s.add_cmd(15, "QueryEntry", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(16, "GetFileSystemAttribute", [](Request&, Response& rsp) -> uint32_t {
            rsp.out.insert(rsp.out.end(), 0xC0, uint8_t{0});
            return 0;
        });
    });
}

std::shared_ptr<Service> make_storage_rom(std::shared_ptr<fs::RomfsImage> img) {
    return make_iface("nn::fssrv::sf::IStorage", [img](Service& s) {
        s.add_cmd(0, "Read", [img](Request& req, Response&) -> uint32_t {
            const int64_t off = req.arg<int64_t>(0);
            const int64_t size = req.arg<int64_t>(8);
            if (off < 0 || size < 0) return fs::rc::kOutOfRange;
            if (size == 0) return 0;
            const ipc::Buffer out = req.out_buf(0);
            if (!out.addr || out.size < static_cast<uint64_t>(size)) return fs::rc::kInvalidArgument;
            if (static_cast<uint64_t>(off) + static_cast<uint64_t>(size) > img->size()) return fs::rc::kOutOfRange;
            return img->read(static_cast<uint64_t>(off), reinterpret_cast<void*>(static_cast<uintptr_t>(out.addr)), static_cast<uint64_t>(size)) ? 0 : fs::rc::kGenericFailure;
        });
        s.add_cmd(1, "Write", [](Request&, Response&) -> uint32_t { return kUnsupportedWrite; });
        s.add_cmd(2, "Flush", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(3, "SetSize", [](Request&, Response&) -> uint32_t { return kUnsupportedWrite; });
        s.add_cmd(4, "GetSize", [img](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(static_cast<int64_t>(img->size()));
            return 0;
        });
        s.add_cmd(5, "OperateRange", [](Request&, Response& rsp) -> uint32_t {
            rsp.out.insert(rsp.out.end(), 0x40, uint8_t{0});
            return 0;
        });
    });
}

std::shared_ptr<fs::RomfsImage> rom_image() {
    std::lock_guard<std::mutex> lk(st().mu);
    if (st().rom) return st().rom;
    if (st().rom_failed) return nullptr;
    auto img = std::make_shared<fs::RomfsImage>();
    std::string err;
    const std::string dir = romfs_dir();
    std::vector<std::string> overlays;
    for (const RomfsMod& m : installed_mods()) {
        if (m.has_exefs) NXLOG_I("mods", "%s: exefs patches are applied by Set up (fefw_build.py), not at start-up", m.name.c_str());
        if (!m.romfs.empty()) {
            NXLOG_I("mods", "%s: RomFS from %s", m.name.c_str(), m.romfs.c_str());
            overlays.push_back(m.romfs);
        }
    }
    if (!img->build(dir, &err, overlays)) {
        NXLOG_E("fs", "RomFS folder '%s' cannot be served: %s (set [fs] romfs)", dir.c_str(), err.c_str());
        st().rom_failed = true;
        return nullptr;
    }
    const fs::RomfsImage::Stats s = img->stats();
    NXLOG_I("fs", "RomFS image from %s: %u files, %u directories, %llu bytes of data, image size %llu", dir.c_str(), s.files, s.dirs,
            static_cast<unsigned long long>(s.data_bytes), static_cast<unsigned long long>(img->size()));
    if (!overlays.empty()) NXLOG_I("mods", "%zu RomFS mod(s), %u files from mods", overlays.size(), s.overlay_files);
    st().rom = img;
    return img;
}

// The RomFS image of the add-on content with data id `id` (installed_aoc()), built on first use. Null when it is not installed or its
// folder cannot be served.
std::shared_ptr<fs::RomfsImage> aoc_image(uint64_t id) {
    std::lock_guard<std::mutex> lk(st().mu);
    auto it = st().aoc_rom.find(id);
    if (it != st().aoc_rom.end()) return it->second;
    for (const AddOnContent& a : installed_aoc()) {
        if (a.data_id != id) continue;
        auto img = std::make_shared<fs::RomfsImage>();
        std::string err;
        if (!img->build(a.romfs, &err)) {
            NXLOG_E("fs", "Add-on content %016llX: folder '%s' cannot be served: %s", static_cast<unsigned long long>(id), a.romfs.c_str(),
                    err.c_str());
            return nullptr;
        }
        const fs::RomfsImage::Stats s = img->stats();
        NXLOG_I("fs", "Add-on content %016llX (index %u) from %s: %u files, image size %llu", static_cast<unsigned long long>(id), a.index,
                a.romfs.c_str(), s.files, static_cast<unsigned long long>(img->size()));
        st().aoc_rom[id] = img;
        return img;
    }
    NXLOG_W("fs", "Add-on content %016llX is not installed (see [aoc] dir)", static_cast<unsigned long long>(id));
    return nullptr;
}

std::shared_ptr<Service> make_event_notifier() {
    auto ev = std::make_shared<ServiceEvent>(false);
    return make_iface("nn::fssrv::sf::IEventNotifier", [ev](Service& s) {
        s.add_cmd(0, "GetEventHandle", [ev](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<Service> make_device_operator() {
    return make_iface("nn::fssrv::sf::IDeviceOperator", [](Service& s) {
        s.add_cmd(0, "IsSdCardInserted", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(200, "IsGameCardInserted", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
    });
}

std::shared_ptr<Service> make_multi_commit() {
    auto list = std::make_shared<std::vector<std::shared_ptr<Service>>>();
    return make_iface("nn::fssrv::sf::IMultiCommitManager", [list](Service& s) {
        s.add_cmd(1, "Add", [list](Request& req, Response&) -> uint32_t {
            if (!req.in_services.empty() && req.in_services[0]) list->push_back(req.in_services[0]);
            return 0;
        });
        s.add_cmd(2, "Commit", [list](Request&, Response&) -> uint32_t {
            // The file systems were added as objects; each one commits through its own Commit command.
            for (auto& fsys : *list) {
                ipc::Request r;
                r.cmd = 10;
                ipc::Response rr;
                const uint32_t res = fsys->handle(r, rr);
                if (res) return res;
            }
            return 0;
        });
    });
}

// SaveDataInfo (0x60 bytes): Switchbrew "Filesystem services".
void fill_info(uint8_t* e, const std::string& key) {
    std::memset(e, 0, 0x60);
    const SaveMeta m = read_meta(key);
    uint8_t attr[0x40] = {};
    for (size_t i = 0; i < 0x40 && i * 2 + 1 < m.attr_hex.size(); ++i) attr[i] = static_cast<uint8_t>(std::strtoul(m.attr_hex.substr(i * 2, 2).c_str(), nullptr, 16));
    const uint64_t id = save_id_of(key);
    std::memcpy(e, &id, 8);
    e[8] = 1;            // SaveDataSpaceId::User
    e[9] = attr[32];     // type
    std::memcpy(e + 0x10, attr + 8, 16);   // user id
    std::memcpy(e + 0x20, attr + 24, 8);   // system save data id
    std::memcpy(e + 0x28, attr, 8);        // application id
    std::memcpy(e + 0x30, &m.size, 8);
    std::memcpy(e + 0x38, attr + 34, 2);   // index
    e[0x3A] = attr[33];                    // rank
}

void collect_saves(std::vector<std::string>* keys) {
    std::error_code ec;
    const stdfs::path base = stdfs::u8path(save_dir());
    for (stdfs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".meta") continue;
        const auto u8rel = it->path().lexically_relative(base).generic_u8string();
        std::string rel(u8rel.begin(), u8rel.end());
        rel.resize(rel.size() - 5);
        keys->push_back(rel);
    }
    std::sort(keys->begin(), keys->end());
}

std::shared_ptr<Service> make_info_reader() {
    auto keys = std::make_shared<std::vector<std::string>>();
    collect_saves(keys.get());
    {
        std::lock_guard<std::mutex> lk(st().mu);
        for (const std::string& k : *keys) st().ids[save_id_of(k)] = k;
    }
    auto pos = std::make_shared<size_t>(0);
    return make_iface("nn::fssrv::sf::ISaveDataInfoReader", [keys, pos](Service& s) {
        s.add_cmd(0, "ReadSaveDataInfo", [keys, pos](Request& req, Response& rsp) -> uint32_t {
            const ipc::Buffer out = req.out_buf(0);
            const size_t cap = static_cast<size_t>(out.size / 0x60);
            size_t n = 0;
            while (n < cap && *pos < keys->size()) fill_info(reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out.addr)) + 0x60 * n++, (*keys)[(*pos)++]);
            rsp.push<int64_t>(static_cast<int64_t>(n));
            return 0;
        });
    });
}

// ---- fsp-srv ----------------------------------------------------------------------------------------------------------------------

// The common tail of the save data opens: space id 1 (User) and 100/0 (system) are served; the SD card spaces are absent.
uint32_t open_save(uint8_t space, const SaveAttr& a, bool read_only, std::shared_ptr<Service>* out) {
    if (space == 2 || space == 4) return fs::rc::kPortSdCardNoDevice;
    const std::string key = save_key(a);
    // The running title's device save data (type 3) exists before it starts on the console: the system creates it at launch from
    // control.nacp's DeviceSaveDataSize (Ryujinx: EnsureApplicationSaveData when the application is loaded). Here it is made on the
    // first open, when the title profile gives a device save size.
    const TitleProfile& p = profile();
    if (a.type == 3 && a.application_id == p.program_id && p.device_save_size > 0 && !save_exists(key)) {
        SaveAttr d;
        d.application_id = p.program_id;
        d.type = 3;
        std::memcpy(d.raw, &d.application_id, 8);
        d.raw[32] = 3;
        create_save(d, static_cast<int64_t>(p.device_save_size), static_cast<int64_t>(p.device_save_journal), p.save_data_owner_id);
    }
    // Likewise the account save data of the user the title starts with (the console's launch path ensures it for the preselected
    // user; Ryujinx EnsureApplicationSaveData with the current profile): some titles mount it without EnsureSaveData first
    // (Mario Kart World: nn::fs::MountSaveData(name, uid) right at boot, and a failure aborts).
    if (a.type == 1 && (a.application_id == p.program_id || a.application_id == 0) && p.account_save_size > 0 && !save_exists(key)) {
        uint64_t hi = 0, lo = 0;
        std::memcpy(&hi, a.uid, 8);
        std::memcpy(&lo, a.uid + 8, 8);
        if (hi == p.user_uid_hi && lo == p.user_uid_lo) {
            NXLOG_I("fs", "OpenSaveDataFileSystem %s: creating the account save data of the starting user", key.c_str());
            fsapi::ensure_account_save(hi, lo, nullptr);
        }
    }
    if (!save_exists(key)) {
        NXLOG_I("fs", "OpenSaveDataFileSystem %s: no such save data", key.c_str());
        return fs::rc::kTargetNotFound;
    }
    auto v = open_save_volume(key, read_only);
    if (!v) return fs::rc::kGenericFailure;
    diag_note_file("save", key.c_str());
    *out = make_filesystem(v, "save:" + key);
    return 0;
}

std::shared_ptr<Service> make_fsp() {
    auto s = std::make_shared<Service>("fsp-srv");
    s->add_cmd(1, "SetCurrentProcess", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(2, "OpenDataFileSystemByCurrentProcess", [](Request&, Response& rsp) -> uint32_t {
        if (nup::is_nup(romfs_dir())) return fs::rc::kTargetNotFound;    // a .nup is served as the RomFS image (OpenDataStorage) only
        auto v = std::make_shared<Volume>(romfs_dir(), false, true, 0);
        if (!v->ok()) return fs::rc::kTargetNotFound;
        rsp.push_object(make_filesystem(v, "rom:"));
        return 0;
    });
    s->add_cmd(8, "OpenFileSystemWithId", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(9, "OpenDataFileSystemByProgramId", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(11, "OpenBisFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(18, "OpenSdCardFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kPortSdCardNoDevice; });
    s->add_cmd(19, "FormatSdCardFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kPortSdCardNoDevice; });
    s->add_cmd(22, "CreateSaveDataFileSystem", [](Request& req, Response&) -> uint32_t {
        if (req.in_size < 0x90) return fs::rc::kInvalidArgument;
        const SaveAttr a = parse_attr(req.in);
        const int64_t size = req.arg<int64_t>(0x40), journal = req.arg<int64_t>(0x48);
        const uint64_t owner = req.arg<uint64_t>(0x58);
        return create_save(a, size, journal, owner);
    });
    s->add_cmd(23, "CreateSaveDataFileSystemBySystemSaveDataId", [](Request& req, Response&) -> uint32_t {
        if (req.in_size < 0x80) return fs::rc::kInvalidArgument;
        const SaveAttr a = parse_attr(req.in);
        return create_save(a, req.arg<int64_t>(0x40), req.arg<int64_t>(0x48), req.arg<uint64_t>(0x58));
    });
    s->add_cmd(25, "DeleteSaveDataFileSystemBySaveDataSpaceId", [](Request& req, Response&) -> uint32_t {
        std::string key;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            auto it = st().ids.find(req.arg<uint64_t>(8));
            if (it == st().ids.end()) return fs::rc::kTargetNotFound;
            key = it->second;
            st().saves.erase(key);
        }
        std::error_code ec;
        stdfs::remove_all(root_of(key), ec);
        stdfs::remove(meta_of(key), ec);
        return 0;
    });
    s->add_cmd(27, "IsExFatSupported", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    s->add_cmd(28, "DeleteSaveDataFileSystemBySaveDataAttribute", [](Request& req, Response&) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        const std::string key = save_key(parse_attr(req.in + 8));
        if (!save_exists(key)) return fs::rc::kTargetNotFound;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            st().saves.erase(key);
        }
        std::error_code ec;
        stdfs::remove_all(root_of(key), ec);
        stdfs::remove(meta_of(key), ec);
        return 0;
    });
    s->add_cmd(32, "ExtendSaveDataFileSystem", [](Request& req, Response&) -> uint32_t {
        std::string key;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            auto it = st().ids.find(req.arg<uint64_t>(8));
            if (it == st().ids.end()) return fs::rc::kTargetNotFound;
            key = it->second;
        }
        SaveMeta m = read_meta(key);
        m.size = std::max<int64_t>(m.size, req.arg<int64_t>(16));
        m.journal = std::max<int64_t>(m.journal, req.arg<int64_t>(24));
        write_meta(key, m);
        return 0;
    });
    s->add_cmd(33, "DeleteCacheStorage", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(34, "GetCacheStorageSize", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(51, "OpenSaveDataFileSystem", [](Request& req, Response& rsp) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        std::shared_ptr<Service> fsys;
        const uint32_t r = open_save(req.arg<uint8_t>(0), parse_attr(req.in + 8), false, &fsys);
        if (r) return r;
        rsp.push_object(fsys);
        return 0;
    });
    s->add_cmd(52, "OpenSaveDataFileSystemBySystemSaveDataId", [](Request& req, Response& rsp) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        std::shared_ptr<Service> fsys;
        const uint32_t r = open_save(req.arg<uint8_t>(0), parse_attr(req.in + 8), false, &fsys);
        if (r) return r;
        rsp.push_object(fsys);
        return 0;
    });
    s->add_cmd(53, "OpenReadOnlySaveDataFileSystem", [](Request& req, Response& rsp) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        std::shared_ptr<Service> fsys;
        const uint32_t r = open_save(req.arg<uint8_t>(0), parse_attr(req.in + 8), true, &fsys);
        if (r) return r;
        rsp.push_object(fsys);
        return 0;
    });
    auto extra = [](Request& req, Response&, uint64_t id) -> uint32_t {   // SaveDataExtraData (0x200 bytes) into the output buffer
        std::string key;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            auto it = st().ids.find(id);
            if (it == st().ids.end()) return fs::rc::kTargetNotFound;
            key = it->second;
        }
        const SaveMeta m = read_meta(key);
        uint8_t e[0x200] = {};
        for (size_t i = 0; i < 0x40 && i * 2 + 1 < m.attr_hex.size(); ++i) e[i] = static_cast<uint8_t>(std::strtoul(m.attr_hex.substr(i * 2, 2).c_str(), nullptr, 16));
        std::memcpy(e + 0x40, &m.owner, 8);
        std::memcpy(e + 0x58, &m.size, 8);       // available size
        std::memcpy(e + 0x60, &m.journal, 8);    // journal size
        buf_write(req.out_buf(0), e, sizeof e);
        return 0;
    };
    s->add_cmd(57, "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId", [extra](Request& req, Response& rsp) -> uint32_t { return extra(req, rsp, req.arg<uint64_t>(8)); });
    s->add_cmd(58, "ReadSaveDataFileSystemExtraData", [extra](Request& req, Response& rsp) -> uint32_t { return extra(req, rsp, req.arg<uint64_t>(0)); });
    s->add_cmd(59, "WriteSaveDataFileSystemExtraData", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(66, "WriteSaveDataFileSystemExtraData2", [](Request&, Response&) -> uint32_t { return 0; });
    auto reader = [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_info_reader());
        return 0;
    };
    s->add_cmd(60, "OpenSaveDataInfoReader", reader);
    s->add_cmd(61, "OpenSaveDataInfoReaderBySaveDataSpaceId", reader);
    s->add_cmd(62, "OpenSaveDataInfoReaderOnlyCacheStorage", reader);
    s->add_cmd(100, "OpenImageDirectoryFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(110, "OpenContentStorageFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    auto rom_storage = [](Request&, Response& rsp) -> uint32_t {
        auto img = rom_image();
        if (!img) return fs::rc::kTargetNotFound;
        rsp.push_object(make_storage_rom(img));
        return 0;
    };
    s->add_cmd(200, "OpenDataStorageByCurrentProcess", rom_storage);
    s->add_cmd(201, "OpenDataStorageByProgramId", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    // OpenDataStorageByDataId(u8 storage id, u64 data id): nn::fs::MountAddOnContent. Only installed add-on content is served.
    s->add_cmd(202, "OpenDataStorageByDataId", [](Request& req, Response& rsp) -> uint32_t {
        auto img = aoc_image(req.arg<uint64_t>(8));
        if (!img) return fs::rc::kTargetNotFound;
        rsp.push_object(make_storage_rom(img));
        return 0;
    });
    s->add_cmd(203, "OpenPatchDataStorageByCurrentProcess", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(205, "OpenDataStorageWithProgramIndex", rom_storage);
    s->add_cmd(400, "OpenDeviceOperator", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_device_operator());
        return 0;
    });
    for (uint32_t id : {500u, 501u, 510u}) {
        s->add_cmd(id, id == 500 ? "OpenSdCardDetectionEventNotifier" : id == 501 ? "OpenGameCardDetectionEventNotifier" : "OpenSystemDataUpdateEventNotifier",
                   [](Request&, Response& rsp) -> uint32_t {
                       rsp.push_object(make_event_notifier());
                       return 0;
                   });
    }
    s->add_cmd(511, "NotifySystemDataUpdateEvent", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(600, "SetCurrentPosixTime", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(601, "QuerySaveDataTotalSize", [](Request& req, Response& rsp) -> uint32_t {
        rsp.push<int64_t>(req.arg<int64_t>(0) + req.arg<int64_t>(8));
        return 0;
    });
    s->add_cmd(620, "SetSdCardEncryptionSeed", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(630, "SetSdCardAccessibility", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(631, "IsSdCardAccessible", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    s->add_cmd(800, "GetAndClearFileSystemProxyErrorInfo", [](Request&, Response& rsp) -> uint32_t {
        rsp.out.insert(rsp.out.end(), 0x80, uint8_t{0});
        return 0;
    });
    s->add_cmd(1001, "SetSaveDataSize", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1003, "DisableAutoSaveDataCreation", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1004, "SetGlobalAccessLogMode", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1005, "GetGlobalAccessLogMode", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        return 0;
    });
    s->add_cmd(1006, "OutputAccessLogToSdCard", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1009, "GetAndClearMemoryReportInfo", [](Request&, Response& rsp) -> uint32_t {
        rsp.out.insert(rsp.out.end(), 0x80, uint8_t{0});
        return 0;
    });
    s->add_cmd(1011, "GetProgramIndexForAccessLog", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        rsp.push<uint32_t>(0);
        return 0;
    });
    s->add_cmd(1012, "GetFsStackUsage", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        return 0;
    });
    s->add_cmd(1013, "UnsetSaveDataRootPath", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1016, "FlushAccessLogOnSdCard", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1017, "OutputApplicationInfoAccessLog", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1200, "OpenMultiCommitManager", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_multi_commit());
        return 0;
    });
    return s;
}

}  // namespace

void register_fs() {
    add("fsp-srv", [] { return make_fsp(); });
    NXLOG_I("fs", "fsp-srv: RomFS from '%s', save data in '%s', %zu add-on content(s) in '%s'", romfs_dir().c_str(), save_dir().c_str(),
            installed_aoc().size(), aoc_dir().c_str());
}

void fs_reset() {
    std::lock_guard<std::mutex> lk(st().mu);
    st().rom.reset();
    st().rom_failed = false;
    st().aoc_rom.clear();
    st().saves.clear();
    st().ids.clear();
}

namespace fsapi {

uint32_t ensure_account_save(uint64_t uid_hi, uint64_t uid_lo, bool* created) {
    if (created) *created = false;
    SaveAttr a;
    a.application_id = profile().program_id;
    std::memcpy(a.uid, &uid_hi, 8);
    std::memcpy(a.uid + 8, &uid_lo, 8);
    a.type = 1;
    std::memcpy(a.raw, &a.application_id, 8);
    std::memcpy(a.raw + 8, a.uid, 16);
    a.raw[32] = 1;
    const TitleProfile& p = profile();
    const uint32_t r = create_save(a, static_cast<int64_t>(p.account_save_size), static_cast<int64_t>(p.account_save_journal), p.save_data_owner_id);
    if (r == fs::rc::kPathAlreadyExists) return 0;
    if (r == 0 && created) *created = true;
    return r;
}

uint32_t account_save_size(uint64_t uid_hi, uint64_t uid_lo, int64_t* size, int64_t* journal) {
    SaveAttr a;
    std::memcpy(a.uid, &uid_hi, 8);
    std::memcpy(a.uid + 8, &uid_lo, 8);
    a.type = 1;
    a.application_id = profile().program_id;
    const std::string key = save_key(a);
    if (!save_exists(key)) return fs::rc::kTargetNotFound;
    const SaveMeta m = read_meta(key);
    if (size) *size = m.size;
    if (journal) *journal = m.journal;
    return 0;
}

}  // namespace fsapi

}  // namespace nxrt::services
