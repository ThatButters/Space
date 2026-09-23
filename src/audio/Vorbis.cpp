// stb_vorbis is a single C file: its implementation is built here once, and other files include it with
// STB_VORBIS_HEADER_ONLY for the declarations. Its warnings are its own (C4701 comes from the optimiser,
// after any push/pop, so it is disabled for the whole file).
#pragma warning(disable : 4701 4703)
#pragma warning(push, 0)
#include <stb_vorbis.c>
#pragma warning(pop)
