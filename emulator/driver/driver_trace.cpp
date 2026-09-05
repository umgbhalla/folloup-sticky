#include "epaper_panel.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace { struct Trace { int full=0, partial=0, old=0, current=0; unsigned char plane=0; bool stuck=false; int dc=0; std::vector<unsigned char> bytes; } t; }
int gpio_config(const gpio_config_t*) { return ESP_OK; }
int gpio_set_level(gpio_num_t pin, int level) { if (pin == 2) t.dc = level; return ESP_OK; }
int gpio_get_level(gpio_num_t) { return t.stuck ? 1 : 0; }
esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int) { return ESP_OK; }
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t* out) { *out=&t; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t* tr) {
    const auto* p = static_cast<const unsigned char*>(tr->tx_buffer); const int n = tr->length / 8;
    if (t.dc == 0 && n == 1) t.plane = p[0];
    else if (t.dc != 0 && t.plane == 0x22 && n == 1) { if (p[0] == 0xFF) ++t.partial; else if (p[0] == 0xF7 || p[0] == 0xD7) ++t.full; }
    else if (t.dc != 0 && t.plane == 0x24) t.current += n;
    else if (t.dc != 0 && t.plane == 0x26) t.old += n;
    t.bytes.insert(t.bytes.end(), p, p+n); return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t) { return ESP_OK; }
esp_err_t spi_bus_free(spi_host_device_t) { return ESP_OK; }

static EpaperPanelConfig cfg(unsigned timeout=10, int bytes=16) { EpaperPanelConfig c{}; c.cs=1; c.dc=2; c.rst=3; c.busy=4; c.mosi=5; c.miso=6; c.sck=7; c.buffer_len=bytes; c.busy_timeout_ms=timeout; return c; }
static EpaperPanel make_panel() { return EpaperPanel(16, 8, cfg()); }
static void seed(EpaperPanel& p) { assert(p.Initialize()==ESP_OK); p.framebuffer()[0]=0; assert(p.RefreshFullBase()==ESP_OK); }
static void mutate(EpaperPanel& p, int n=0) { p.framebuffer()[n%16] ^= 1; }
static void reset_trace() { t.full=t.partial=t.old=t.current=0; t.bytes.clear(); }
static void print_json(const char* s, esp_err_t e=ESP_OK) { std::printf("{\"scenario\":\"%s\",\"result\":\"%s\",\"full_activations\":%d,\"partial_activations\":%d,\"old_plane_bytes\":%d,\"new_plane_bytes\":%d}\n",s,esp_err_to_name(e),t.full,t.partial,t.old,t.current); }

static bool next_token(std::ifstream& f, std::string& out) {
    out.clear(); char c;
    while (f.get(c)) { if (c == '#') { f.ignore(1 << 20, '\n'); continue; } if (c > ' ') { out.push_back(c); break; } }
    while (f.get(c) && c > ' ') out.push_back(c);
    return !out.empty();
}
static size_t native_offset(int x, int y) { return static_cast<size_t>(y) * 100u + static_cast<size_t>(x) / 8u; }
static bool read_pbm(const char* path, std::vector<unsigned char>& pixels) {
    std::ifstream f(path, std::ios::binary); std::string magic, sw, sh;
    if (!f || !next_token(f, magic) || magic != "P4" || !next_token(f, sw) || !next_token(f, sh)) return false;
    if (sw != "480" || sh != "800") return false;
    std::vector<unsigned char> portrait(48000); if (!f.read(reinterpret_cast<char*>(portrait.data()), portrait.size())) return false;
    char trailing = 0; if (f.get(trailing)) return false;
    pixels.assign(48000, 0xFF); // PBM 1 is black; the SSD1677 framebuffer uses 0 for black.
    for (int py = 0; py < 800; ++py) for (int px = 0; px < 480; ++px) {
        const bool black = (portrait[static_cast<size_t>(py) * 60u + px / 8] & (0x80u >> (px % 8))) != 0;
        const int nx = py, ny = 479 - px;
        const size_t at = native_offset(nx, ny); const unsigned char mask = 0x80u >> (nx % 8);
        if (black) pixels[at] &= static_cast<unsigned char>(~mask); else pixels[at] |= mask;
    }
    return true;
}
static int frames_mode(int argc, char** argv) {
    bool routed = false; int first = 2;
    if (first < argc && !std::strcmp(argv[first], "--changed")) { routed = true; ++first; }
    if (first >= argc) return 2;
    std::vector<std::vector<unsigned char>> frames;
    for (int i = first; i < argc; ++i) { frames.emplace_back(); if (!read_pbm(argv[i], frames.back())) { std::fprintf(stderr, "invalid 480x800 P4 PBM: %s\n", argv[i]); return 2; } }
    // Portrait source corners map to the native landscape framebuffer corners.
    assert(native_offset(0, 479) == 47900 && native_offset(799, 0) == 99);
    auto p = EpaperPanel(800, 480, cfg(10, 48000)); assert(p.Initialize() == ESP_OK);
    std::memcpy(p.framebuffer(), frames[0].data(), frames[0].size()); reset_trace(); assert(p.RefreshFullBase() == ESP_OK);
    std::printf("{\"mode\":\"%s\",\"frame\":0,\"event\":\"full\",\"partial_count\":%d,\"old_plane_bytes\":%d,\"new_plane_bytes\":%d}\n", routed ? "changed" : "partial", p.partial_refresh_count(), t.old, t.current);
    int total_full = t.full, total_partial = t.partial, total_noop = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        std::memcpy(p.framebuffer(), frames[i].data(), frames[i].size()); reset_trace();
        const esp_err_t err = routed ? p.RefreshChangedRegion() : p.RefreshPartialFullScreen(); assert(err == ESP_OK);
        const char* event = t.full ? "full" : t.partial ? "partial" : "noop"; if (!t.full && !t.partial) ++total_noop;
        total_full += t.full; total_partial += t.partial;
        std::printf("{\"mode\":\"%s\",\"frame\":%zu,\"event\":\"%s\",\"partial_count\":%d,\"old_plane_bytes\":%d,\"new_plane_bytes\":%d}\n", routed ? "changed" : "partial", i, event, p.partial_refresh_count(), t.old, t.current);
    }
    std::printf("{\"mode\":\"%s\",\"event\":\"totals\",\"frames\":%zu,\"full\":%d,\"partial\":%d,\"noop\":%d}\n", routed ? "changed" : "partial", frames.size(), total_full, total_partial, total_noop);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--frames")) return frames_mode(argc, argv);
    const char* s=argc>1?argv[1]:"baseline";
    if (!std::strcmp(s,"baseline")) { auto p=make_panel(); seed(p); reset_trace(); mutate(p); assert(p.RefreshChangedRegion()==ESP_OK); assert(t.old==16&&t.current==16); print_json(s); return 0; }
    if (!std::strcmp(s,"noop")) { auto p=make_panel(); seed(p); reset_trace(); assert(p.RefreshChangedRegion()==ESP_OK); assert(t.bytes.empty()); print_json(s); return 0; }
    if (!std::strcmp(s,"eight_then_full")) { auto p=make_panel(); seed(p); reset_trace(); for(int i=0;i<8;i++){mutate(p,i);assert(p.RefreshChangedRegion()==ESP_OK);} assert(p.partial_refresh_count()==8); mutate(p,8); assert(p.RefreshChangedRegion()==ESP_OK); assert(t.old==16*9&&t.current==16*9); print_json(s); return 0; }
    if (!std::strcmp(s,"busy_timeout")) { t.stuck=true; auto p=make_panel(); assert(p.Initialize()==ESP_OK); p.framebuffer()[0]=0; esp_err_t e=p.RefreshFullBase(); assert(e==ESP_ERR_TIMEOUT); print_json(s,e); t.stuck=false; return 0; }
    if (!std::strcmp(s,"equivalence")) { auto run=[](bool routed){auto p=make_panel();seed(p);reset_trace();mutate(p);assert((routed?p.RefreshChangedRegion():p.RefreshPartialFullScreen())==ESP_OK);return t.bytes;}; auto a=run(false),b=run(true); assert(a==b); print_json(s); return 0; }
    std::fprintf(stderr,"usage: %s [baseline|noop|eight_then_full|busy_timeout|equivalence]\n",argv[0]); return 2;
}
