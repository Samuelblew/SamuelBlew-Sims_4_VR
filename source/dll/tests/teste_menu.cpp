// teste_menu.cpp: desenha o menu VR fora do jogo (PNG) e testa tokens e acerto dos botões.
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include <openxr/openxr.h>
#define FPC_TESTE_MENU
static void Log(const char* fmt, ...) { va_list a; va_start(a, fmt); printf("  [log] "); vprintf(fmt, a); printf("\n"); va_end(a); }
#include "../vr_menu.inl"
#include "../vr_status.inl"
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size);
    std::vector<uint8_t> buf(size); auto* info = (Gdiplus::ImageCodecInfo*)buf.data(); Gdiplus::GetImageEncoders(num, size, info);
    for (UINT j = 0; j < num; ++j) if (wcscmp(info[j].MimeType, format) == 0) { *pClsid = info[j].Clsid; return j; }
    return -1;
}
int main() {
    int fails = 0;
    // tokens
    std::vector<std::string> toks = { "s:Bella Goth", "n:3" };
    std::wstring t1 = ApplyTokens(L"Conversar com {0.SimFirstName}", toks);
    std::wstring t2 = ApplyTokens(L"{M0.Ele}{F0.Ela} tem {1.Number} amigos", toks);
    std::wstring t3 = ApplyTokens(L"Abraçar {0.SimName}", toks);
    printf("tokens: '%ls' | '%ls' | '%ls'\n", t1.c_str(), t2.c_str(), t3.c_str());
    if (t1 != L"Conversar com Bella" || t2 != L"Ele tem 3 amigos" || t3 != L"Abraçar Bella Goth") { printf("  FALHA tokens\n"); ++fails; }
    // menu de exemplo
    g_menuCats = { { 101, L"Amigável" }, { 102, L"Engraçado" }, { 103, L"Romântico" } };
    g_menuItems = {
        { 1, L"Conversar com Bella", false, 0, 5 }, { 2, L"Abraçar", false, 0, 4 }, { 3, L"Apresentar-se", true, 0, 3 },
        { 4, L"Elogiar a aparência", false, 101, 0 }, { 5, L"Contar piada", false, 102, 0 }, { 6, L"Flertar", false, 103, 0 } };
    g_menuCurCat = 0; g_menuHover = 2;
    MenuBuildButtons(); MenuRender();
    printf("botoes no topo: %d (esperado 3 categorias + 3 opcoes + centro = 7)\n", (int)g_menuButtons.size());
    if (g_menuButtons.size() != 7) ++fails;
    // acerto: centro de cada botão
    for (int i = 0; i < (int)g_menuButtons.size(); ++i) {
        int k = MenuButtonAt(g_menuButtons[i].cx / MENU_TEX, g_menuButtons[i].cy / MENU_TEX);
        if (k != i) { printf("  FALHA acerto botao %d -> %d\n", i, k); ++fails; }
    }
    if (MenuButtonAt(0.02f, 0.02f) != -1) { printf("  FALHA canto deveria ser vazio\n"); ++fails; }
    // salvar PNG (canal alfa) para conferir
    std::vector<uint32_t> px = g_menuPixels;
    Gdiplus::Bitmap out(MENU_TEX, MENU_TEX, PixelFormat32bppPARGB);
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, MENU_TEX, MENU_TEX);
    out.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd);
    for (int y = 0; y < MENU_TEX; ++y) for (int x = 0; x < MENU_TEX; ++x) {
        uint32_t p = px[y * MENU_TEX + x];   // RGBA -> BGRA
        ((uint32_t*)((uint8_t*)bd.Scan0 + y * bd.Stride))[x] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
    }
    out.UnlockBits(&bd);
    CLSID png; GetEncoderClsid(L"image/png", &png);
    out.Save(L"menu_teste.png", &png, nullptr);
    // submenu
    g_menuCurCat = 101; g_menuHover = -1; MenuBuildButtons();
    printf("botoes no submenu Amigavel: %d (1 opcao + centro)\n", (int)g_menuButtons.size());
    if (g_menuButtons.size() != 2) ++fails;
    // relógio de pulso
    StatusRender("nome=Bella Goth\nhumor_hash=0\nhora=14:35\ndia=3\ndinheiro=18250\nnec_Fome=0.82\nnec_Energia=0.35\nnec_Bexiga=0.10\nnec_Higiene=0.66\nnec_Social=0.95\nnec_Diversao=0.50");
    {
        Gdiplus::Bitmap o2(ST_W, ST_H, PixelFormat32bppPARGB); Gdiplus::BitmapData b2; Gdiplus::Rect r2(0, 0, ST_W, ST_H);
        o2.LockBits(&r2, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &b2);
        for (int y = 0; y < ST_H; ++y) for (int x = 0; x < ST_W; ++x) { uint32_t q = g_stPixels[y * ST_W + x];
            ((uint32_t*)((uint8_t*)b2.Scan0 + y * b2.Stride))[x] = (q & 0xff00ff00) | ((q & 0xff) << 16) | ((q >> 16) & 0xff); }
        o2.UnlockBits(&b2); CLSID png2; GetEncoderClsid(L"image/png", &png2); o2.Save(L"relogio_teste.png", &png2, nullptr);
        printf("relogio: %s\n", g_stPixels.empty() ? "FALHA" : "desenhado");
        if (g_stPixels.empty()) ++fails;
    }
    printf("\n%s (%d falhas)\n", fails ? "HA FALHAS" : "TUDO OK", fails);
    return fails;
}
