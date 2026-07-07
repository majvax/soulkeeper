// src/client/stb_vorbis_impl.cpp
//
// The one TU that compiles stb_vorbis (OGG decoder for music tracks).
// audio.cpp includes it with STB_VORBIS_HEADER_ONLY for the declarations.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <stb_vorbis.c>
