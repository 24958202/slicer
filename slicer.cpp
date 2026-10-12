// Image Slicer: C++23, saucer 7.0.2, saucer-desktop 4.2.0.
// Build using the accompanying CMakeLists.txt.
#include <saucer/smartview.hpp>
#include <saucer/modules/desktop.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

// Bridge return types need external linkage for Glaze reflection on AppleClang.
// Keep this aggregate outside the anonymous namespace.
struct rollover_asset
{
    std::string relative;
    std::string data;
};

namespace
{
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr std::size_t max_file_bytes = 128ULL * 1024 * 1024;
    constexpr auto decode_table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        for (std::size_t i = 0; i < alphabet.size(); ++i)
            t[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
        return t;
    }();

    std::expected<std::string, std::string> base64_decode(std::string_view input)
    {
        if (input.size() > ((max_file_bytes + 2) / 3) * 4)
            return std::unexpected("Encoded file exceeds the 128 MiB limit");
        std::string s;
        s.reserve(input.size());
        for (unsigned char c : input)
        {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            s.push_back(static_cast<char>(c));
        }
        if (s.size() % 4 != 0) return std::unexpected("Invalid base64 length");
        std::string out;
        out.reserve(s.size() / 4 * 3);
        for (std::size_t i = 0; i < s.size(); i += 4)
        {
            const bool p2 = s[i + 2] == '=', p3 = s[i + 3] == '=';
            const int a = decode_table[static_cast<unsigned char>(s[i])];
            const int b = decode_table[static_cast<unsigned char>(s[i + 1])];
            const int c = p2 ? 0 : decode_table[static_cast<unsigned char>(s[i + 2])];
            const int d = p3 ? 0 : decode_table[static_cast<unsigned char>(s[i + 3])];
            if (a < 0 || b < 0 || c < 0 || d < 0 || (p2 && !p3) ||
                ((p2 || p3) && i + 4 != s.size()) ||
                (p2 && (b & 15)) || (!p2 && p3 && (c & 3)))
                return std::unexpected("Invalid base64 data or padding");
            const auto n = (static_cast<std::uint32_t>(a) << 18) |
                           (static_cast<std::uint32_t>(b) << 12) |
                           (static_cast<std::uint32_t>(c) << 6) |
                           static_cast<std::uint32_t>(d);
            out.push_back(static_cast<char>((n >> 16) & 255));
            if (!p2) out.push_back(static_cast<char>((n >> 8) & 255));
            if (!p3) out.push_back(static_cast<char>(n & 255));
        }
        if (out.size() > max_file_bytes) return std::unexpected("Decoded file too large");
        return out;
    }

    std::string base64_encode(std::string_view in)
    {
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        for (std::size_t i = 0; i < in.size(); i += 3)
        {
            const std::uint32_t b0 = static_cast<unsigned char>(in[i]);
            const std::uint32_t b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
            const std::uint32_t b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
            const auto n = (b0 << 16) | (b1 << 8) | b2;
            out.push_back(alphabet[(n >> 18) & 63]);
            out.push_back(alphabet[(n >> 12) & 63]);
            out.push_back(i + 1 < in.size() ? alphabet[(n >> 6) & 63] : '=');
            out.push_back(i + 2 < in.size() ? alphabet[n & 63] : '=');
        }
        return out;
    }

    fs::path from_utf8(std::string_view s)
    {
        return fs::path{std::u8string{reinterpret_cast<const char8_t*>(s.data()), s.size()}};
    }
    std::string to_utf8(const fs::path& p)
    {
        const auto u = p.generic_u8string();
        return {u.begin(), u.end()};
    }
    fs::path home_dir()
    {
        for (const char* key : {"HOME", "USERPROFILE"})
            if (const auto* v = std::getenv(key); v && *v) return from_utf8(v);
        return fs::current_path();
    }
    bool starts_with_ci(std::string_view value, std::string_view prefix)
    {
        if (value.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(value[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
        return true;
    }
    int hex_digit(unsigned char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    std::string file_url_path(std::string_view url)
    {
        // Accept file:///path, file://localhost/path, and file:/path.
        // Do not percent-decode ordinary filesystem paths.
        auto path = url.substr(5); // caller has checked the file: scheme
        if (path.starts_with("//"))
        {
            path.remove_prefix(2);
            const auto slash = path.find('/');
            if (slash == std::string_view::npos)
                throw std::runtime_error("File URL is missing an absolute path");
            const auto host = path.substr(0, slash);
            if (!host.empty() && !(host.size() == 9 && starts_with_ci(host, "localhost")))
                throw std::runtime_error("Only local file URLs are supported");
            path.remove_prefix(slash);
        }
        if (path.empty() || path.front() != '/')
            throw std::runtime_error("File URL must contain an absolute path");
        if (path.find_first_of("?#") != std::string_view::npos)
            throw std::runtime_error("File URL contains a query/fragment; encode filename ? or # characters");
        std::string decoded;
        decoded.reserve(path.size());
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(path[i]);
            if (c == '%')
            {
                if (i + 2 >= path.size()) throw std::runtime_error("Incomplete file URL escape");
                const int h = hex_digit(static_cast<unsigned char>(path[i + 1]));
                const int l = hex_digit(static_cast<unsigned char>(path[i + 2]));
                if (h < 0 || l < 0) throw std::runtime_error("Invalid file URL escape");
                c = static_cast<unsigned char>((h << 4) | l);
                i += 2;
            }
            if (c == 0) throw std::runtime_error("NUL byte in file URL");
            decoded.push_back(static_cast<char>(c)); // '+' is a literal plus
        }
#ifdef _WIN32
        // Local drive URL: file:///C:/Users/... -> C:/Users/...
        if (decoded.size() >= 3 && decoded[0] == '/' &&
            std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':')
            decoded.erase(0, 1);
#endif
        return decoded;
    }
    fs::path expand(std::string_view s)
    {
        if (s.find('\0') != std::string_view::npos)
            throw std::runtime_error("NUL byte in filesystem path");
        if (starts_with_ci(s, "file:")) return from_utf8(file_url_path(s));
        if (s.find("://") != std::string_view::npos)
            throw std::runtime_error("Expected a filesystem path or local file:// URL");
        if (s == "~") return home_dir();
        if (s.starts_with("~/") || s.starts_with("~\\"))
            return home_dir() / from_utf8(s.substr(2));
        return from_utf8(s);
    }
    std::expected<std::string, std::string> normalize_path(const std::string& text)
    {
        try
        {
            if (text.empty()) return std::unexpected("Empty filesystem path");
            return to_utf8(fs::absolute(expand(text)));
        }
        catch (const std::exception& e) { return std::unexpected(std::string{e.what()}); }
    }
    std::expected<std::string, std::string> picker_path(const fs::path& p)
    {
        // u8string(), not generic_u8string(): preserve URL slashes until parsed.
        try
        {
            const auto raw = p.u8string();
            return normalize_path(std::string{raw.begin(), raw.end()});
        }
        catch (const std::exception& e) { return std::unexpected(std::string{e.what()}); }
    }
    std::string mime_for(const fs::path& p)
    {
        auto ext = to_utf8(p.extension());
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".gif") return "image/gif";
        if (ext == ".webp") return "image/webp";
        if (ext == ".bmp") return "image/bmp";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".avif") return "image/avif";
        return "application/octet-stream";
    }

    // Only the application's generated output names may be written.
    bool allowed_output(std::string_view rel)
    {
        if (rel == "index.html" || rel == "slices.css" ||
            rel == "index-over.html" || rel == "slices-over.css") return true;
        if (!rel.starts_with("image/slice_")) return false;
        rel.remove_prefix(12);
        std::size_t n = 0;
        while (n < rel.size() && rel[n] >= '0' && rel[n] <= '9') ++n;
        return n >= 2 && (rel.substr(n) == ".png" || rel.substr(n) == "_hover.png");
    }
    bool allowed_rollover(std::string_view rel)
    {
        if (rel.starts_with("image/slice_")) rel.remove_prefix(12);
        else if (rel.starts_with("image/slice")) rel.remove_prefix(11);
        else return false;
        std::size_t n = 0;
        while (n < rel.size() && rel[n] >= '0' && rel[n] <= '9') ++n;
        return n >= 2 && rel.substr(n) == "_over.jpg";
    }
    fs::path safe_join(const fs::path& root, std::string_view rel)
    {
        if (!allowed_output(rel) && !allowed_rollover(rel))
            throw std::runtime_error("Illegal asset filename");
        const auto base = fs::weakly_canonical(root);
        auto path = base;
        for (const auto& part : from_utf8(rel))
        {
            path /= part;
            // Reject symlink outputs, including the image directory.
            if (fs::is_symlink(fs::symlink_status(path)))
                throw std::runtime_error("Refusing symlink output: " + to_utf8(path));
        }
        return path;
    }
    std::expected<std::string, std::string> read_image(const std::string& name)
    {
        try
        {
            if (name.empty()) return std::unexpected("Enter an image path");
            const auto p = expand(name);
            if (!fs::is_regular_file(p)) return std::unexpected("Not a regular file: " + name);
            const auto size = fs::file_size(p);
            if (!size || size > max_file_bytes)
                return std::unexpected("Image must be between 1 byte and 128 MiB");
            std::ifstream in{p, std::ios::binary};
            if (!in) return std::unexpected("Cannot open: " + name);
            std::string data(static_cast<std::size_t>(size), '\0');
            in.read(data.data(), static_cast<std::streamsize>(data.size()));
            if (!in) return std::unexpected("Cannot read image completely: " + name);
            return "data:" + mime_for(p) + ";base64," + base64_encode(data);
        }
        catch (const std::exception& e) { return std::unexpected(std::string{e.what()}); }
    }
    std::expected<rollover_asset, std::string> read_rollover(const std::string& dir, int number)
    {
        try
        {
            if (dir.empty() || number < 1 || number > 1000)
                return std::unexpected("Invalid rollover request");
            auto n = std::to_string(number);
            if (n.size() < 2) n.insert(0, "0");
            const auto canonical = "image/slice_" + n + "_over.jpg";
            const auto compact = "image/slice" + n + "_over.jpg";
            const auto root = expand(dir);
            const auto a = safe_join(root, canonical), b = safe_join(root, compact);
            const bool has_a = fs::is_regular_file(a), has_b = fs::is_regular_file(b);
            if (has_a && has_b)
                return std::unexpected("Ambiguous rollover: keep only " + canonical + " or " + compact);
            if (!has_a && !has_b)
                return std::unexpected("Missing rollover: place " + canonical + " (or " + compact + ") in the export folder");
            auto data = read_image(to_utf8(has_a ? a : b));
            if (!data) return std::unexpected(data.error());
            return rollover_asset{has_a ? canonical : compact, std::move(*data)};
        }
        catch (const std::exception& e) { return std::unexpected(std::string{e.what()}); }
    }
    std::expected<void, std::string> save_file(
        const std::string& dir, const std::string& rel, const std::string& b64)
    {
        try
        {
            if (dir.empty()) return std::unexpected("Empty export folder");
            // External Photoshop artwork is read-only; never overwrite it.
            if (!allowed_output(rel)) return std::unexpected("Illegal output filename");
            auto bytes = base64_decode(b64);
            if (!bytes) return std::unexpected(bytes.error());
            const auto target = safe_join(expand(dir), rel);
            fs::create_directories(target.parent_path());
            std::ofstream out{target, std::ios::binary | std::ios::trunc};
            if (!out) return std::unexpected("Cannot write: " + to_utf8(target));
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            out.close();
            if (!out) return std::unexpected("Write/close failed: " + to_utf8(target));
            return {};
        }
        catch (const std::exception& e) { return std::unexpected(std::string{e.what()}); }
    }
    std::expected<bool, std::string> export_exists(const std::string& dir, int count, bool external = false)
    {
        try
        {
            if (dir.empty() || count < 1 || count > 1000)
                return std::unexpected("Invalid export request");
            const auto root = expand(dir);
            bool exists = fs::exists(safe_join(root, "index.html")) ||
                          fs::exists(safe_join(root, "slices.css"));
            if (external)
                exists = fs::exists(safe_join(root, "index-over.html")) ||
                         fs::exists(safe_join(root, "slices-over.css")) || exists;
            for (int i = 1; i <= count; ++i)
            {
                auto n = std::to_string(i);
                if (n.size() < 2) n.insert(0, "0");
                for (const auto* suffix : {".png", "_hover.png"})
                    exists = fs::exists(safe_join(root, "image/slice_" + n + suffix)) || exists;
            }
            return exists;
        }
        catch (const std::exception& e) { return std::unexpected(std::string{e.what()}); }
    }
}

namespace ui
{
constexpr std::string_view page = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Image Slicer</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#15171c;--panel:#1d2027;--line:#2d313b;--fg:#e6e9ef;--mut:#8b93a3;--acc:#4da3ff}
*{box-sizing:border-box}[hidden]{display:none!important}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);font:13px/1.4 system-ui,sans-serif;overflow:hidden}
#app{display:flex;flex-direction:column;height:100%}
header{display:flex;flex-wrap:wrap;gap:8px 18px;align-items:center;padding:8px 12px;background:var(--panel);border-bottom:1px solid var(--line)}
.grp{display:flex;gap:6px;align-items:center}
button,select,input[type=text],input[type=number]{font:inherit;color:var(--fg);background:#262a33;border:1px solid var(--line);border-radius:6px;padding:5px 9px}
button{cursor:pointer}button:hover:not(:disabled){border-color:var(--acc)}
button.on{background:var(--acc);color:#04121f}button.primary{background:#2f9e5f;color:white}
button:disabled{opacity:.45;cursor:default}#path{width:180px}#outDir{width:220px}
main{flex:1;display:flex;min-height:0}#stage{flex:1;position:relative;min-width:0;background:repeating-conic-gradient(#1a1d23 0 25%,#171a1f 0 50%) 0 0/20px 20px}
#stage.drop{outline:2px dashed var(--acc);outline-offset:-8px}
#cv{position:absolute;inset:0;width:100%;height:100%;touch-action:none}
#empty{position:absolute;inset:0;display:grid;place-items:center;text-align:center;color:var(--mut);pointer-events:none}
aside{width:290px;padding:14px;background:var(--panel);border-left:1px solid var(--line);overflow:auto}
aside h3{margin:0;font-size:15px}aside label{display:block;margin-top:12px;color:var(--mut)}
aside label>input[type=text],aside label>select,aside label>input[type=range],aside label>input[type=number]{display:block;width:100%;margin-top:4px}
aside label.chk{display:flex;gap:6px;align-items:center}hr{border:0;border-top:1px solid var(--line);margin:16px 0 4px}
.muted{color:var(--mut)}#swapThumb{display:block;max-width:100%;max-height:90px;margin-top:8px}
footer{display:flex;justify-content:space-between;gap:12px;padding:5px 12px;background:var(--panel);color:var(--mut)}
#pv{position:fixed;inset:0;z-index:9;background:#000c;display:flex;flex-direction:column;gap:8px;padding:16px}
#pv iframe{flex:1;width:100%;border:0;background:white;border-radius:8px}
dialog{max-width:480px;background:var(--panel);color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:20px}
dialog::backdrop{background:#000a}
</style></head><body><div id="app">
<header>
<div class="grp"><button id="open">Open image…</button><input id="path" type="text" placeholder="…or type a file path"><button id="loadPath">Load</button></div>
<div class="grp"><button id="tSlice" title="S">Slice (Drag)</button><button id="tSelect" title="C">Select</button><button id="undo" title="Ctrl/Cmd+Z">Undo</button><button id="reset">Reset</button><button id="fitBtn" title="F">Fit</button><button id="previewBtn">Preview</button></div>
<div class="grp"><input id="outDir" type="text" placeholder="Export folder"><button id="browse">Browse…</button><label class="grp"><input id="resp" type="checkbox"> Responsive width</label><button id="exportBtn" class="primary">Export</button></div>
</header><main><div id="stage"><canvas id="cv"></canvas><div id="empty">Open or drop an image to start</div></div>
<aside><div id="noSel" class="muted"><h3>No slice selected</h3>
<p>Drag a rectangle with <b>Slice</b> to carve out a region. The surrounding image becomes separate slices, without gaps or overlaps.</p>
<p>Use <b>Select</b> to assign links and hover effects.</p><p>Wheel = zoom. Middle-drag, Alt-drag, or drag outside the image = pan.</p></div>
<div id="form" hidden><h3 id="pTitle"></h3><div id="pInfo" class="muted"></div>
<label>Link URL<input id="url" type="text" placeholder="https://example.com"></label>
<label class="chk"><input id="newTab" type="checkbox" checked> Open in new tab</label><hr>
<label>Mouse-over effect<select id="fxType"></select></label>
<label id="rowAmt"><span id="amtName"></span>: <b id="amtVal"></b><input id="fxAmt" type="range"></label>
<label id="rowColor">Color <input id="fxColor" type="color"></label>
<label id="rowMs">Transition (ms)<input id="fxMs" type="number" min="0" max="5000" step="50"></label>
<div id="rowSwap"><label>Hover image (chosen images are stretched to slice size)</label>
<label class="chk"><input id="useOver" type="checkbox"> Use Photoshop *_over.jpg file</label>
<p id="overHint" class="muted" hidden></p>
<div class="grp"><button id="swapBtn">Choose…</button><button id="swapClear">Remove</button></div><img id="swapThumb" alt="Hover image" hidden></div>
</div></aside></main><footer><span id="status" role="status"></span><span id="coords"></span></footer>
<div id="pv" hidden><button id="pvClose">Close preview (Esc)</button><iframe id="pvf" title="Slice preview" sandbox></iframe></div>
<dialog id="confirm"><p id="confirmText"></p><div class="grp"><button id="confirmYes">Continue</button><button id="confirmNo">Cancel</button></div></dialog>
</div><script>
'use strict';
const $=s=>document.querySelector(s),clamp=(v,a,b)=>Math.min(b,Math.max(a,v));
const call=(fn,...args)=>window.saucer?window.saucer.call(fn,args):Promise.reject(new Error('saucer bridge not found'));
const status=(t,err=false)=>{ $('#status').textContent=t;$('#status').style.color=err?'#ff7b7b':''; };
const bind=(s,e,f)=>$(s).addEventListener(e,f);
const MIN=10,MAX_PIECES=1000,MAX_PIXELS=64*1024*1024;
const FX={none:{label:'None'},brighten:{label:'Brighten',name:'Strength',def:25,min:0,max:100,unit:'%'},
 darken:{label:'Darken',name:'Strength',def:30,min:0,max:90,unit:'%'},
 grayscale:{label:'Grayscale',name:'Amount',def:100,min:0,max:100,unit:'%'},
 fade:{label:'Fade out',name:'Amount',def:40,min:0,max:90,unit:'%'},
 zoom:{label:'Zoom in',name:'Scale',def:8,min:1,max:40,unit:'%'},
 overlay:{label:'Color overlay',name:'Opacity',def:35,min:0,max:100,unit:'%',color:true},
 outline:{label:'Highlight border',name:'Width',def:4,min:1,max:30,unit:'px',color:true},
 swap:{label:'Swap image on hover',swap:true}};
const defFx=()=>({type:'none',amount:25,color:'#ffcc00',ms:200,swap:null,external:false});
const S={img:null,W:0,H:0,pieces:[],sel:null,tool:'slice',m:null,nextId:1,hist:[],snap:[],busy:false};
const V={s:1,ox:0,oy:0},cv=$('#cv'),ctx=cv.getContext('2d'),stage=$('#stage');
let dpr=1,sliceDrag=null,panDrag=null;
const newPiece=(x,y,w,h)=>({id:S.nextId++,x,y,w,h,url:'',newTab:true,fx:defFx()});
const order=()=>[...S.pieces].sort((a,b)=>a.y-b.y||a.x-b.x||a.id-b.id);
const cur=()=>S.pieces.find(p=>p.id===S.sel);
const at=pt=>pt&&S.pieces.find(p=>pt.x>=p.x&&pt.x<p.x+p.w&&pt.y>=p.y&&pt.y<p.y+p.h);
function ask(text){return new Promise(resolve=>{
 const d=$('#confirm');$('#confirmText').textContent=text;
 const done=value=>{d.close();$('#confirmYes').onclick=null;$('#confirmNo').onclick=null;d.oncancel=null;resolve(value);};
 $('#confirmYes').onclick=()=>done(true);$('#confirmNo').onclick=()=>done(false);
 d.oncancel=e=>{e.preventDefault();done(false);};d.showModal();
});}
async function runBusy(fn){
 if(S.busy||sliceDrag||panDrag)return;
 commit();S.busy=true;updateUi();
 try{await fn();}catch(e){status(e.message||String(e),true);}
 finally{S.busy=false;refreshPanel();updateUi();draw();}
}
function loadImg(src){return new Promise((resolve,reject)=>{
 const im=new Image();im.onload=()=>resolve(im);im.onerror=()=>reject(new Error('Cannot decode image in this webview'));im.src=src;
});}
function validateImage(im){
 const w=im.naturalWidth,h=im.naturalHeight;
 if(!w||!h||w>32767||h>32767||w*h>MAX_PIXELS)throw new Error('Image dimensions exceed the 64-megapixel / 32767-pixel-edge limit');
 return im;
}
async function setImage(src){
 const im=validateImage(await loadImg(src)),w=im.naturalWidth,h=im.naturalHeight;
 // Freeze animated formats once so slices cannot capture different frames.
 const frozen=document.createElement('canvas');frozen.width=w;frozen.height=h;
 frozen.getContext('2d').drawImage(im,0,0,w,h);canvasPng(frozen);
 S.img=frozen;S.W=w;S.H=h;S.sel=null;S.nextId=1;
 S.pieces=[newPiece(0,0,S.W,S.H)];S.hist=[];S.snap=structuredClone(S.pieces);S.m=null;
 $('#empty').hidden=true;resize();fit();refreshPanel();
 status(`Loaded ${S.W}×${S.H}px. Drag a rectangle to slice.`);
}
async function loadFile(f){
 if(f.size>128*1024*1024)throw new Error('File exceeds 128 MiB');
 const src=await new Promise((resolve,reject)=>{const r=new FileReader();r.onload=()=>resolve(r.result);r.onerror=()=>reject(new Error('Cannot read dropped file'));r.readAsDataURL(f);});
 await setImage(src);
}
function resize(){dpr=window.devicePixelRatio||1;cv.width=Math.max(1,Math.round(stage.clientWidth*dpr));cv.height=Math.max(1,Math.round(stage.clientHeight*dpr));draw();}
function fit(){if(!S.img)return;const w=stage.clientWidth,h=stage.clientHeight;
 V.s=Math.max(.001,Math.min(Math.max(1,w-48)/S.W,Math.max(1,h-48)/S.H));V.ox=(w-S.W*V.s)/2;V.oy=(h-S.H*V.s)/2;draw();}
function draw(){
 const w=stage.clientWidth,h=stage.clientHeight;ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,w,h);if(!S.img)return;
 ctx.imageSmoothingEnabled=V.s<1;ctx.drawImage(S.img,V.ox,V.oy,S.W*V.s,S.H*V.s);
 const ord=order(),hov=at(S.m);
 for(let i=0;i<ord.length;i++){
  const p=ord[i],x=V.ox+p.x*V.s,y=V.oy+p.y*V.s,pw=p.w*V.s,ph=p.h*V.s;
  if(p.id===S.sel||p===hov&&S.tool==='select'){ctx.fillStyle=p.id===S.sel?'#4da3ff38':'#ffffff20';ctx.fillRect(x,y,pw,ph);}
  ctx.lineWidth=3;ctx.strokeStyle='#0009';ctx.strokeRect(x,y,pw,ph);ctx.lineWidth=1;ctx.strokeStyle=p.id===S.sel?'#4da3ff':'#fffffff0';ctx.strokeRect(x,y,pw,ph);
  if(pw>44&&ph>22){const tag=`#${i+1}${p.url?' [link]':''}${p.fx.type!=='none'?' *':''}`;ctx.font='12px system-ui';ctx.textBaseline='middle';ctx.fillStyle='#000a';ctx.fillRect(x+4,y+4,ctx.measureText(tag).width+10,18);ctx.fillStyle='white';ctx.fillText(tag,x+9,y+13);}
 }
 if(sliceDrag){const d=sliceDrag,x=Math.min(d.a.x,d.b.x),y=Math.min(d.a.y,d.b.y);ctx.save();ctx.fillStyle='#4da3ff33';ctx.strokeStyle='#4da3ff';ctx.lineWidth=2;ctx.setLineDash([4,4]);ctx.fillRect(V.ox+x*V.s,V.oy+y*V.s,Math.abs(d.b.x-d.a.x)*V.s,Math.abs(d.b.y-d.a.y)*V.s);ctx.strokeRect(V.ox+x*V.s,V.oy+y*V.s,Math.abs(d.b.x-d.a.x)*V.s,Math.abs(d.b.y-d.a.y)*V.s);ctx.restore();}
}
function commit(){
 if(JSON.stringify(S.snap)===JSON.stringify(S.pieces))return;
 S.hist.push(S.snap);if(S.hist.length>100)S.hist.shift();S.snap=structuredClone(S.pieces);updateUi();
}
function undo(){if(S.busy)return;commit();if(!S.hist.length)return;
 S.pieces=S.hist.pop();S.snap=structuredClone(S.pieces);if(!cur())S.sel=null;refreshPanel();updateUi();draw();status('Undo');}
// Remove the chosen rectangle from every intersecting piece, keeping up to four
// disjoint surrounding fragments. All fragments retain their parent's settings.
function carve(x,y,w,h){
 const result=[],oldNext=S.nextId;
 for(const p of S.pieces){
  const l=Math.max(x,p.x),t=Math.max(y,p.y),r=Math.min(x+w,p.x+p.w),b=Math.min(y+h,p.y+p.h);
  if(l>=r||t>=b){result.push(p);continue;}
  const add=(xx,yy,ww,hh)=>{if(ww>0&&hh>0)result.push({...p,id:S.nextId++,x:xx,y:yy,w:ww,h:hh,fx:{...p.fx}});};
  add(p.x,p.y,p.w,t-p.y);add(p.x,b,p.w,p.y+p.h-b);
  add(p.x,t,l-p.x,b-t);add(r,t,p.x+p.w-r,b-t);
 }
 if(result.length+1>MAX_PIECES){S.nextId=oldNext;throw new Error('Too many slices (limit 1000). Undo or reset first.');}
 const p=newPiece(x,y,w,h);result.push(p);S.pieces=result;S.sel=p.id;commit();refreshPanel();status(`${S.pieces.length} non-overlapping slices`);
}
const point=e=>{const r=cv.getBoundingClientRect();return{x:(e.clientX-r.left-V.ox)/V.s,y:(e.clientY-r.top-V.oy)/V.s};};
const bounded=p=>({x:clamp(p.x,0,S.W),y:clamp(p.y,0,S.H)});
const inside=p=>p.x>=0&&p.x<S.W&&p.y>=0&&p.y<S.H;
const setCursor=()=>{cv.style.cursor=panDrag?'grabbing':S.tool==='slice'?'crosshair':'default';};
cv.addEventListener('pointerdown',e=>{
 if(!S.img||S.busy||$('#pv').hidden===false||sliceDrag||panDrag)return;
 if(e.button!==0&&e.button!==1)return;commit();const pt=point(e);cv.setPointerCapture(e.pointerId);e.preventDefault();
 if(e.button===1||e.altKey||!inside(pt)){panDrag={id:e.pointerId,x:e.clientX,y:e.clientY,ox:V.ox,oy:V.oy};setCursor();return;}
 if(S.tool==='slice')sliceDrag={id:e.pointerId,a:bounded(pt),b:bounded(pt)};
 else{S.sel=at(pt)?.id??null;refreshPanel();draw();}
});
cv.addEventListener('pointermove',e=>{
 if(!S.img)return;
 if(panDrag&&panDrag.id===e.pointerId){V.ox=panDrag.ox+e.clientX-panDrag.x;V.oy=panDrag.oy+e.clientY-panDrag.y;}
 else if(sliceDrag&&sliceDrag.id===e.pointerId)sliceDrag.b=bounded(point(e));
 S.m=inside(point(e))?point(e):null;
 $('#coords').textContent=`${S.m?`x ${Math.floor(S.m.x)} y ${Math.floor(S.m.y)} | `:''}${S.W}×${S.H}px | ${Math.round(V.s*100)}%`;draw();
});
cv.addEventListener('pointerup',e=>{
 if(panDrag?.id===e.pointerId)panDrag=null;
 if(sliceDrag?.id===e.pointerId){
  const d=sliceDrag;d.b=bounded(point(e));sliceDrag=null;
  const x=Math.round(Math.min(d.a.x,d.b.x)),y=Math.round(Math.min(d.a.y,d.b.y));
  const r=Math.round(Math.max(d.a.x,d.b.x)),b=Math.round(Math.max(d.a.y,d.b.y));
  if(r-x>=MIN&&b-y>=MIN){try{carve(x,y,r-x,b-y);}catch(err){status(err.message,true);}}
  else status('Slice must be at least 10×10 image pixels');
 }
 if(cv.hasPointerCapture(e.pointerId))cv.releasePointerCapture(e.pointerId);setCursor();draw();
});
const cancelDrag=()=>{sliceDrag=null;panDrag=null;setCursor();draw();};
cv.addEventListener('pointercancel',cancelDrag);cv.addEventListener('lostpointercapture',cancelDrag);
cv.addEventListener('pointerleave',()=>{if(!sliceDrag&&!panDrag){S.m=null;draw();}});
cv.addEventListener('contextmenu',e=>e.preventDefault());
cv.addEventListener('wheel',e=>{
 if(!S.img||sliceDrag||panDrag)return;e.preventDefault();const r=cv.getBoundingClientRect(),mx=e.clientX-r.left,my=e.clientY-r.top;
 const delta=e.deltaY*(e.deltaMode===1?16:e.deltaMode===2?stage.clientHeight:1),ns=clamp(V.s*Math.exp(-delta*.0015),.001,32);
 V.ox=mx-(mx-V.ox)*ns/V.s;V.oy=my-(my-V.oy)*ns/V.s;V.s=ns;draw();
},{passive:false});
window.addEventListener('keydown',e=>{
 if($('#confirm').open)return;
 if(e.key==='Escape'){if(!$('#pv').hidden)closePreview();else cancelDrag();return;}
 if(S.busy||sliceDrag||panDrag||!$('#pv').hidden||e.target.closest('input,select,textarea,[contenteditable]'))return;
 const k=e.key.toLowerCase();if((e.ctrlKey||e.metaKey)&&k==='z'){e.preventDefault();undo();return;}if(e.ctrlKey||e.metaKey||e.altKey)return;
 if(k==='s')S.tool='slice';else if(k==='c')S.tool='select';else if(k==='f'){fit();return;}else return;updateUi();draw();
});
function updateUi(){
 $('#tSlice').classList.toggle('on',S.tool==='slice');$('#tSelect').classList.toggle('on',S.tool==='select');
 for(const id of ['open','loadPath','browse','path','outDir','resp'])$('#'+id).disabled=S.busy;
 for(const id of ['tSlice','tSelect','reset','fitBtn','previewBtn','exportBtn'])$('#'+id).disabled=S.busy||!S.img;
 $('#undo').disabled=S.busy||!S.hist.length;
 $('#form').querySelectorAll('input,select,button').forEach(e=>e.disabled=S.busy);
 for(const id of ['swapBtn','swapClear'])$('#'+id).disabled=S.busy||!!cur()?.fx.external;
 setCursor();
}
for(const [k,m] of Object.entries(FX))$('#fxType').add(new Option(m.label,k));
function refreshPanel(){
 const p=cur();$('#noSel').hidden=!!p;$('#form').hidden=!p;if(!p)return;
 const f=p.fx,m=FX[f.type];$('#pTitle').textContent=`Slice #${order().indexOf(p)+1}`;$('#pInfo').textContent=`${p.w}×${p.h}px at (${p.x}, ${p.y})`;
 $('#url').value=p.url;$('#newTab').checked=p.newTab;$('#fxType').value=f.type;$('#rowAmt').hidden=m.min===undefined;
 if(m.min!==undefined){const r=$('#fxAmt');$('#amtName').textContent=m.name;r.min=m.min;r.max=m.max;r.value=f.amount;$('#amtVal').textContent=f.amount+m.unit;}
 $('#rowColor').hidden=!m.color;$('#fxColor').value=f.color;$('#rowMs').hidden=f.type==='none';$('#fxMs').value=f.ms;
 $('#rowSwap').hidden=!m.swap;$('#useOver').checked=!!f.external;$('#overHint').hidden=!f.external;
 const n=order().indexOf(p)+1,digits=String(n).padStart(2,'0');
 $('#overHint').textContent=`Reads image/slice_${digits}_over.jpg (or slice${digits}_over.jpg) from the export folder. Must be ${p.w}×${p.h}px. Exports index-over.html + slices-over.css. Finish slicing before editing numbered files.`;
 $('#swapThumb').hidden=!!f.external||!f.swap;if(f.swap&&!f.external)$('#swapThumb').src=f.swap;else $('#swapThumb').removeAttribute('src');
 for(const id of ['swapBtn','swapClear'])$('#'+id).disabled=S.busy||!!f.external;
}
bind('#url','input',()=>{const p=cur();if(p&&!S.busy){p.url=$('#url').value;draw();}});bind('#url','change',commit);
bind('#newTab','change',()=>{const p=cur();if(p&&!S.busy){p.newTab=$('#newTab').checked;commit();}});
bind('#fxType','change',()=>{const p=cur();if(!p||S.busy)return;p.fx.type=$('#fxType').value;const m=FX[p.fx.type];if(m.def!==undefined)p.fx.amount=m.def;refreshPanel();commit();draw();});
bind('#fxAmt','input',()=>{const p=cur();if(p&&!S.busy){p.fx.amount=+$('#fxAmt').value;$('#amtVal').textContent=p.fx.amount+FX[p.fx.type].unit;}});bind('#fxAmt','change',commit);
bind('#fxColor','input',()=>{const p=cur();if(p&&!S.busy)p.fx.color=$('#fxColor').value;});bind('#fxColor','change',commit);
bind('#fxMs','change',()=>{const p=cur();if(p&&!S.busy){p.fx.ms=clamp(Number($('#fxMs').value)||0,0,5000);$('#fxMs').value=p.fx.ms;commit();}});
bind('#useOver','change',()=>{const p=cur();if(!p||S.busy)return;p.fx.external=$('#useOver').checked;commit();refreshPanel();draw();});
bind('#swapBtn','click',()=>runBusy(async()=>{const p=cur();if(!p||p.fx.external)return;const path=await call('pick_image');if(!path)return;const src=await call('read_image',path);validateImage(await loadImg(src));p.fx.swap=src;commit();status('Hover image loaded');}));
bind('#swapClear','click',()=>{const p=cur();if(p&&!S.busy&&!p.fx.external){p.fx.swap=null;commit();refreshPanel();}});
const esc=s=>String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
const nm=n=>`slice_${String(n).padStart(2,'0')}`;
// Allow relative links and ordinary web/mail/phone links, not script/data schemes.
function safeUrl(value){const u=value.trim();if(!u)return '';if(/[\u0000-\u0020\u007f]/.test(u))throw new Error('Link contains whitespace/control characters; encode spaces as %20');
 const scheme=u.match(/^([a-z][a-z0-9+.-]*):/i);if(scheme&&!/^(https?|mailto|tel)$/i.test(scheme[1]))throw new Error('Unsupported link scheme: '+scheme[1]);return u;}
function canvasPng(c){const u=c.toDataURL('image/png');if(!u.startsWith('data:image/png;base64,'))throw new Error('Webview cannot export this canvas size');return u;}
function sliceDataURL(p){const c=document.createElement('canvas');c.width=p.w;c.height=p.h;c.getContext('2d').drawImage(S.img,p.x,p.y,p.w,p.h,0,0,p.w,p.h);return canvasPng(c);}
async function swapDataURL(p){const im=await loadImg(p.fx.swap),c=document.createElement('canvas');c.width=p.w;c.height=p.h;c.getContext('2d').drawImage(im,0,0,p.w,p.h);return canvasPng(c);}
function layout(ord){
 const xs=[...new Set([0,S.W,...ord.flatMap(p=>[p.x,p.x+p.w])])].sort((a,b)=>a-b);
 const ys=[...new Set([0,S.H,...ord.flatMap(p=>[p.y,p.y+p.h])])].sort((a,b)=>a-b);
 const rows=ys.length-1,cols=xs.length-1;
 if(rows*cols>1000000)throw new Error('Table grid too complex. Undo or reset some slices.');
 const grid=Array.from({length:rows},()=>Array(cols).fill(-1)),spans=[];
 ord.forEach((p,i)=>{
  if(![p.x,p.y,p.w,p.h].every(Number.isInteger)||p.w<=0||p.h<=0||p.x<0||p.y<0||p.x+p.w>S.W||p.y+p.h>S.H)throw new Error('Invalid slice geometry');
  safeUrl(p.url);const c1=xs.indexOf(p.x),c2=xs.indexOf(p.x+p.w),r1=ys.indexOf(p.y),r2=ys.indexOf(p.y+p.h);spans.push({c1,c2,r1,r2});
  for(let r=r1;r<r2;r++)for(let c=c1;c<c2;c++){if(grid[r][c]!==-1)throw new Error('Overlapping slices');grid[r][c]=i;}
 });
 if(grid.some(row=>row.includes(-1)))throw new Error('Uncovered image region');return {xs,ys,rows,cols,grid,spans};
}
function effectCss(p,n,hasHover=!!p.fx.swap||!!p.fx.external){
 const f=p.fx,a=Number(f.amount),t=clamp(Number(f.ms)||0,0,5000),c='.s'+n;
 const img=`${c}:hover .main,${c}:focus-visible .main`;
 switch(f.type){
 case 'brighten':return `${c} .main{transition:filter ${t}ms}${img}{filter:brightness(${1+a/100})}`;
 case 'darken':return `${c} .main{transition:filter ${t}ms}${img}{filter:brightness(${1-a/100})}`;
 case 'grayscale':return `${c} .main{transition:filter ${t}ms}${img}{filter:grayscale(${a/100})}`;
 case 'fade':return `${c} .main{transition:opacity ${t}ms}${img}{opacity:${1-a/100}}`;
 case 'zoom':return `${c} .main{transition:transform ${t}ms}${img}{transform:scale(${1+a/100})}`;
 case 'overlay':return `${c}::after{content:"";position:absolute;inset:0;background:${f.color};opacity:0;transition:opacity ${t}ms;pointer-events:none}${c}:hover::after,${c}:focus-visible::after{opacity:${a/100}}`;
 case 'outline':return `${c}::after{content:"";position:absolute;inset:0;box-shadow:inset 0 0 0 ${a}px ${f.color};opacity:0;transition:opacity ${t}ms;pointer-events:none}${c}:hover::after,${c}:focus-visible::after{opacity:1}`;
 case 'swap':return hasHover?`${c} .h{position:absolute;inset:0;opacity:0;pointer-events:none;transition:opacity ${t}ms}${c}:hover .h,${c}:focus-visible .h{opacity:1}`:'';
 default:return '';
 }
}
function buildCss(ord,srcs,responsive){
 let css=`html,body{margin:0;padding:0}body{background:white}.frame{position:relative;${responsive?'width:100%;max-width:'+S.W+'px;aspect-ratio:'+S.W+'/'+S.H+';':'width:'+S.W+'px;height:'+S.H+'px;'}}
 table{position:absolute;inset:0;width:100%;height:100%;table-layout:fixed;border-collapse:collapse;border-spacing:0}
 td{padding:0;border:0;position:relative;line-height:0}.s{position:absolute;inset:0;display:block;overflow:hidden;outline-offset:-2px}
 img{display:block;position:absolute;inset:0;width:100%;height:100%;border:0;user-select:none;-webkit-user-drag:none}.s:focus-visible{outline:2px solid #4da3ff;z-index:1}
 `;
 css+=ord.map((p,i)=>effectCss(p,i+1,!!srcs[i].hover)).join('\n');
 css+='\n@media(prefers-reduced-motion:reduce){*,*::after{transition:none!important}}';
 return css+'\n';
}
function buildHtml(ord,srcs,{preview,responsive,stylesheet=null},g=layout(ord)){
 const {xs,ys,rows,cols,grid,spans}=g;
 // Explicit row heights and column widths preserve span-based table geometry.
 const headStyle=stylesheet?`<link rel="stylesheet" href="${esc(stylesheet)}">`:`<style>${buildCss(ord,srcs,responsive)}</style>`;
 let table='<div class="frame"><table role="presentation"><colgroup>';
 for(let c=0;c<cols;c++)table+=`<col style="width:${(xs[c+1]-xs[c])/S.W*100}%">`;
 table+='</colgroup>\n<tbody>\n';
 for(let r=0;r<rows;r++){
  table+=`<tr style="height:${(ys[r+1]-ys[r])/S.H*100}%">`;
  for(let c=0;c<cols;c++){
   const i=grid[r][c],sp=spans[i];if(r!==sp.r1||c!==sp.c1)continue;
   const p=ord[i],url=safeUrl(p.url),tag=url?'a':'div';
   const attrs=url?(preview?` href="#" title="${esc(url)}"`:` href="${esc(url)}"${p.newTab?' target="_blank" rel="noopener noreferrer"':''}`):(srcs[i].hover?` tabindex="0" aria-label="Slice ${i+1}"`:'');
   table+=`<td rowspan="${sp.r2-sp.r1}" colspan="${sp.c2-sp.c1}"><${tag} class="s s${i+1}"${attrs}><img class="main" src="${esc(srcs[i].main)}" alt="${url?'Slice '+(i+1):''}">${srcs[i].hover?`<img class="h" src="${esc(srcs[i].hover)}" alt="" aria-hidden="true">`:''}</${tag}></td>`;
  }
  table+='</tr>\n';
 }
 table+='</tbody></table></div>';
 return `<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Sliced Image</title>${headStyle}</head><body>${table}</body></html>\n`;
}
const b64of=u=>u.slice(u.indexOf(',')+1);
function utf8b64(s){const b=new TextEncoder().encode(s);let bin='';for(let i=0;i<b.length;i+=0x8000)bin+=String.fromCharCode(...b.subarray(i,i+0x8000));return btoa(bin);}
const save=(dir,rel,b64)=>call('save_file',dir,rel,b64);
const externalSwap=p=>p.fx.type==='swap'&&!!p.fx.external;
async function resolveRollovers(ord,dir,keepData){
 const assets=new Map();
 for(let i=0;i<ord.length;i++){
  const p=ord[i];if(!externalSwap(p))continue;
  status(`Checking Photoshop rollover for slice ${i+1}…`);
  try{
   const asset=await call('read_rollover',dir,i+1),im=validateImage(await loadImg(asset.data));
   if(im.naturalWidth!==p.w||im.naturalHeight!==p.h)
    throw new Error(`${asset.relative} is ${im.naturalWidth}×${im.naturalHeight}px; expected ${p.w}×${p.h}px. Keep the Photoshop canvas at the original slice size.`);
   assets.set(i,{relative:asset.relative,data:keepData?asset.data:null});
  }catch(e){throw new Error(`Slice #${i+1}: ${e.message||e}`);}
 }
 return assets;
}
async function doExport(){
 if(!S.img)return;const entered=$('#outDir').value.trim();if(!entered)throw new Error('Enter an export folder first');
 const dir=await call('normalize_path',entered);$('#outDir').value=dir;
 const ord=order(),g=layout(ord),srcs=[],useExternal=ord.some(externalSwap);
 // Validate all external art before writing anything; the original JPEGs stay untouched.
 const rollovers=useExternal?await resolveRollovers(ord,dir,false):new Map();
 if(await call('export_exists',dir,ord.length,useExternal)&&!await ask('Generated files already exist in this folder. Overwrite matching HTML/CSS and generated PNG files? Photoshop *_over.jpg artwork and unrelated files will be left untouched.'))return;
 try{
  for(let i=0;i<ord.length;i++){
   const p=ord[i],n=i+1;status(`Exporting slice ${n}/${ord.length}…`);
   await save(dir,`image/${nm(n)}.png`,b64of(sliceDataURL(p)));
   const hover=p.fx.type==='swap'&&!externalSwap(p)&&p.fx.swap;
   if(hover)await save(dir,`image/${nm(n)}_hover.png`,b64of(await swapDataURL(p)));
   srcs.push({main:`image/${nm(n)}.png`,hover:hover?`image/${nm(n)}_hover.png`:null});
  }
  const responsive=$('#resp').checked;
  await save(dir,'slices.css',utf8b64(buildCss(ord,srcs,responsive)));
  await save(dir,'index.html',utf8b64(buildHtml(ord,srcs,{preview:false,responsive,stylesheet:'slices.css'},g)));
  if(useExternal){
   const overSrcs=srcs.map((s,i)=>({...s,hover:rollovers.has(i)?rollovers.get(i).relative:s.hover}));
   await save(dir,'slices-over.css',utf8b64(buildCss(ord,overSrcs,responsive)));
   await save(dir,'index-over.html',utf8b64(buildHtml(ord,overSrcs,{preview:false,responsive,stylesheet:'slices-over.css'},g)));
  }
  status(`Done: ${ord.length} slices + index.html / slices.css${useExternal?' + index-over.html / slices-over.css':''} written to ${dir}. Photoshop artwork was not modified.`);
 }catch(e){throw new Error('Export failed; some files may have been written. '+(e.message||e));}
}
async function preview(){
 if(!S.img)return;const ord=order(),g=layout(ord),srcs=[],useExternal=ord.some(externalSwap);status('Building preview…');
 let rollovers=new Map();
 if(useExternal){
  const entered=$('#outDir').value.trim();if(!entered)throw new Error('Choose the export folder containing Photoshop *_over.jpg files before previewing');
  const dir=await call('normalize_path',entered);$('#outDir').value=dir;rollovers=await resolveRollovers(ord,dir,true);
 }
 for(let i=0;i<ord.length;i++){
  const p=ord[i],asset=rollovers.get(i);
  srcs.push({main:sliceDataURL(p),hover:asset?asset.data:p.fx.type==='swap'&&p.fx.swap?await swapDataURL(p):null});
 }
 // srcdoc uses inline CSS and data URLs; no file URL access or scripts are needed.
 $('#pvf').srcdoc=buildHtml(ord,srcs,{preview:true,responsive:$('#resp').checked},g);$('#pv').hidden=false;status(`Preview${useExternal?' (Photoshop rollover version)':''}: links do not navigate`);
}
function closePreview(){$('#pv').hidden=true;$('#pvf').srcdoc='';}
async function loadPath(path){const normalized=await call('normalize_path',path);$('#path').value=normalized;await setImage(await call('read_image',normalized));}
bind('#open','click',()=>runBusy(async()=>{const p=await call('pick_image');if(p){$('#path').value=p;await loadPath(p);}}));
bind('#browse','click',()=>runBusy(async()=>{const p=await call('pick_folder');if(p)$('#outDir').value=p;}));
bind('#loadPath','click',()=>runBusy(()=>loadPath($('#path').value.trim())));
bind('#path','keydown',e=>{if(e.key==='Enter')runBusy(()=>loadPath($('#path').value.trim()));});
bind('#tSlice','click',()=>{if(S.busy)return;commit();S.tool='slice';updateUi();draw();});
bind('#tSelect','click',()=>{if(S.busy)return;commit();S.tool='select';updateUi();draw();});bind('#undo','click',undo);
bind('#reset','click',()=>runBusy(async()=>{if(!S.img||!await ask('Reset all slices, links and effects? You can Undo this reset.'))return;
 S.pieces=[newPiece(0,0,S.W,S.H)];S.sel=null;commit();status('Reset to one full-image slice');}));
bind('#fitBtn','click',fit);bind('#previewBtn','click',()=>runBusy(preview));bind('#pvClose','click',closePreview);bind('#exportBtn','click',()=>runBusy(doExport));
['dragenter','dragover'].forEach(ev=>stage.addEventListener(ev,e=>{e.preventDefault();if(!S.busy)stage.classList.add('drop');}));
stage.addEventListener('dragleave',e=>{if(!stage.contains(e.relatedTarget))stage.classList.remove('drop');});
stage.addEventListener('drop',e=>{e.preventDefault();stage.classList.remove('drop');if(S.busy)return;
 const f=[...e.dataTransfer.files].find(f=>f.type.startsWith('image/')||/\.(png|jpe?g|gif|webp|bmp|svg|avif)$/i.test(f.name));
 if(f)runBusy(async()=>{await loadFile(f);$('#path').value=f.name;});else status('Drop a supported image file',true);
});
new ResizeObserver(resize).observe(stage);
call('default_dir').then(d=>{if(!$('#outDir').value)$('#outDir').value=d;}).catch(e=>status(e.message||String(e),true));
updateUi();resize();
</script></body></html>)HTML";
}

coco::stray start(saucer::application* app)
{
    using saucer::modules::picker::type;
    auto desktop = saucer::modules::desktop{app};
    auto window = saucer::window::create(app).value();
    auto webview = saucer::smartview<>::create({.window = window});
    window->set_title("Image Slicer");
    window->set_size({1360, 860});

    webview->expose("default_dir", [] { return to_utf8(home_dir() / "saucer-slices"); });
    webview->expose("normalize_path", &normalize_path);
    webview->expose("read_image", &read_image);
    webview->expose("read_rollover", &read_rollover);
    webview->expose("save_file", &save_file);
    webview->expose("export_exists", &export_exists);
    webview->expose("pick_image", [&]() -> std::expected<std::string, std::string> {
        auto p = desktop.pick<type::file>({});
        if (!p)
        {
            if (p.error().type() == saucer::error::category::platform &&
                p.error().code() == static_cast<int>(std::errc::operation_canceled)) return std::string{};
            return std::unexpected(p.error().message());
        }
        return picker_path(*p);
    });
    webview->expose("pick_folder", [&]() -> std::expected<std::string, std::string> {
        auto p = desktop.pick<type::folder>({});
        if (!p)
        {
            if (p.error().type() == saucer::error::category::platform &&
                p.error().code() == static_cast<int>(std::errc::operation_canceled)) return std::string{};
            return std::unexpected(p.error().message());
        }
        return picker_path(*p);
    });
    webview->embed({{"/index.html", saucer::embedded_file{
        .content = saucer::stash<>::from_str(std::string{ui::page}), .mime = "text/html"}}});
    webview->serve("/index.html");
    window->show();
    // Locals (including exposed callback captures) live until the event loop ends.
    co_await app->finish();
}
int main()
{
    try
    {
        return saucer::application::create({.id = "com.image.slicer"})->run(start);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Image Slicer: " << e.what() << '\n';
        return 1;
    }
}
