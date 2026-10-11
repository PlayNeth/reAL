/* Doppler cases for a DirectMusic 3D audiopath buffer: dopptest (Windows, Microsoft's DirectSound) and test_doppler
 * (reAL's model) print one line per case, "case N freq F cents C". */
typedef struct {
    float lpos[3], lvel[3], ldoppler, ldist;
    float bpos[3], bvel[3];
    unsigned mode, freq;            /* DS3DMODE_*; buffer frequency, 0 = the original */
} dop_case;

static const dop_case dop_cases[] = {
    /* source straight ahead, moving away / towards */
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,10}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,-10}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,-50}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,120}, 0, 0 },
    /* off axis, oblique velocity */
    { {0,0,0}, {0,0,0}, 1, 1,      {3,1,4}, {-7,2,5}, 0, 0 },
    { {1,2,3}, {0,0,0}, 1, 1,      {-4,0.5f,9}, {12.5f,-3,1}, 0, 0 },
    /* moving listener only, both moving the same way, both moving */
    { {0,0,0}, {0,0,20}, 1, 1,     {0,0,10}, {0,0,0}, 0, 0 },
    { {0,0,0}, {0,0,20}, 1, 1,     {0,0,10}, {0,0,20}, 0, 0 },
    { {5,0,0}, {-15,0,3}, 1, 1,    {-5,2,1}, {8,-1,0}, 0, 0 },
    /* Doppler and distance factors */
    { {0,0,0}, {0,0,0}, 2, 1,      {0,0,10}, {0,0,-30}, 0, 0 },
    { {0,0,0}, {0,0,0}, 0.5f, 1,   {0,0,10}, {0,0,-30}, 0, 0 },
    { {0,0,0}, {0,0,0}, 10, 1,     {0,0,10}, {0,0,-30}, 0, 0 },
    { {0,0,0}, {0,0,0}, 0, 1,      {0,0,10}, {0,0,-30}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 0.3048f,{0,0,10}, {0,0,-30}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 3,      {0,0,10}, {0,0,40}, 0, 0 },
    /* beyond the clamp (half the speed of sound either way) */
    { {0,0,0}, {0,0,0}, 10, 1,     {0,0,10}, {0,0,-300}, 0, 0 },
    { {0,0,0}, {0,0,0}, 10, 1,     {0,0,10}, {0,0,300}, 0, 0 },
    /* source on the listener */
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,0}, {0,0,30}, 0, 0 },
    /* head-relative (listener at the origin, still), disabled */
    { {50,0,0}, {0,0,30}, 1, 1,    {0,0,10}, {0,0,-10}, 1, 0 },
    { {50,0,0}, {0,0,30}, 1, 1,    {0,0,10}, {0,0,0}, 1, 0 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,-10}, 2, 0 },
    /* a frequency of the buffer's own, with and without movement */
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,0}, 0, 30000 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,-25}, 0, 30000 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,25}, 0, 11025 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,0}, 2, 44100 },
    /* small speeds: whole-Hz truncation */
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,0.01f}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,-0.01f}, 0, 0 },
    { {0,0,0}, {0,0,0}, 1, 1,      {0,0,10}, {0,0,0.4f}, 0, 0 },
};
#define DOP_NCASES (sizeof(dop_cases) / sizeof(dop_cases[0]))
