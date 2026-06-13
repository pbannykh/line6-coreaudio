//
//  haltest.c - in-process test harness for Line6.driver (no coreaudiod).
//
//  Loads the bundle into this process (dlopen + the Line6_Create factory) and
//  exercises the AudioServerPlugIn interface the way the HAL does:
//    1. property sweep: for every object x selector x scope x element, checks
//       that HasProperty / GetPropertyDataSize / GetPropertyData agree. A
//       mismatch is what makes coreaudiod spin at 100% CPU, so this gates every
//       install.
//    2. IO simulation: StartIO -> GetZeroTimeStamp / DoIO(ReadInput) loop, then
//       checks the captured signal is a continuous tone (needs `line6-daemon
//       daemon --sine` running).
//
//  Build/run:  make haltest && ./haltest
//
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>   // kAudioDevicePropertyStreamConfiguration
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "line6_shm.h"

// Read-only view of the daemon's shm, to learn the channel count.
static line6_shm_t* gShmPeek = NULL;
static UInt32 gChannels = 8;
static void peek_attach(void) {
    int fd = shm_open(LINE6_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) return;
    void* p = mmap(NULL, LINE6_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p != MAP_FAILED) {
        gShmPeek = (line6_shm_t*)p;
        if (gShmPeek->channels > 0 && gShmPeek->channels <= LINE6_MAX_CHANNELS)
            gChannels = gShmPeek->channels;
    }
}

// ---------- mock host interface ----------
static UInt32 gNotifications = 0;
static OSStatus MockPropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress* a)
{ (void)h;(void)o;(void)n;(void)a; gNotifications++; return noErr; }
static OSStatus MockCopyFromStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef* v)
{ (void)h;(void)k; if (v) *v = NULL; return kAudioHardwareUnknownPropertyError; }
static OSStatus MockWriteToStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v)
{ (void)h;(void)k;(void)v; return noErr; }
static OSStatus MockDeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef k)
{ (void)h;(void)k; return noErr; }
static OSStatus MockRequestDeviceConfigurationChange(AudioServerPlugInHostRef h, AudioObjectID d, UInt64 a, void* i)
{ (void)h;(void)d;(void)a;(void)i; return noErr; }

static AudioServerPlugInHostInterface gHost = {
    .PropertiesChanged = MockPropertiesChanged,
    .CopyFromStorage = MockCopyFromStorage,
    .WriteToStorage = MockWriteToStorage,
    .DeleteFromStorage = MockDeleteFromStorage,
    .RequestDeviceConfigurationChange = MockRequestDeviceConfigurationChange,
};

// ---------- selectors the HAL queries ----------
typedef struct { AudioObjectPropertySelector sel; const char* name; } SelDef;
static const SelDef kSelectors[] = {
    { kAudioObjectPropertyBaseClass, "BaseClass" },
    { kAudioObjectPropertyClass, "Class" },
    { kAudioObjectPropertyOwner, "Owner" },
    { kAudioObjectPropertyName, "Name" },
    { kAudioObjectPropertyManufacturer, "Manufacturer" },
    { kAudioObjectPropertyOwnedObjects, "OwnedObjects" },
    { kAudioPlugInPropertyDeviceList, "DeviceList" },
    { kAudioPlugInPropertyBoxList, "BoxList" },
    { kAudioPlugInPropertyClockDeviceList, "ClockDeviceList" },
    { kAudioPlugInPropertyTranslateUIDToDevice, "TranslateUID" },
    { kAudioPlugInPropertyResourceBundle, "ResourceBundle" },
    { kAudioObjectPropertyControlList, "ControlList" },
    { kAudioDevicePropertyDeviceUID, "DeviceUID" },
    { kAudioDevicePropertyModelUID, "ModelUID" },
    { kAudioDevicePropertyTransportType, "TransportType" },
    { kAudioDevicePropertyClockDomain, "ClockDomain" },
    { kAudioDevicePropertyDeviceIsAlive, "IsAlive" },
    { kAudioDevicePropertyDeviceIsRunning, "IsRunning" },
    { kAudioDevicePropertyDeviceCanBeDefaultDevice, "CanDefault" },
    { kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, "CanDefaultSys" },
    { kAudioDevicePropertyLatency, "Latency" },
    { kAudioDevicePropertySafetyOffset, "SafetyOffset" },
    { kAudioDevicePropertyIsHidden, "IsHidden" },
    { kAudioDevicePropertyZeroTimeStampPeriod, "ZTSPeriod" },
    { kAudioDevicePropertyNominalSampleRate, "NominalRate" },
    { kAudioDevicePropertyAvailableNominalSampleRates, "AvailRates" },
    { kAudioDevicePropertyRelatedDevices, "RelatedDevices" },
    { kAudioDevicePropertyStreams, "Streams" },
    { kAudioDevicePropertyStreamConfiguration, "StreamConfig" },
    { kAudioDevicePropertyPreferredChannelsForStereo, "PrefStereo" },
    { kAudioStreamPropertyIsActive, "IsActive" },
    { kAudioStreamPropertyDirection, "Direction" },
    { kAudioStreamPropertyTerminalType, "TerminalType" },
    { kAudioStreamPropertyStartingChannel, "StartingChannel" },
    { kAudioStreamPropertyLatency, "StreamLatency" },
    { kAudioStreamPropertyVirtualFormat, "VirtualFormat" },
    { kAudioStreamPropertyPhysicalFormat, "PhysicalFormat" },
    { kAudioStreamPropertyAvailableVirtualFormats, "AvailVirtFmts" },
    { kAudioStreamPropertyAvailablePhysicalFormats, "AvailPhysFmts" },
};
static const AudioObjectPropertyScope kScopes[] = {
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput,
};
static const char* kScopeNames[] = { "glob", "in  ", "out " };

static int gProblems = 0;

static void sweep(AudioServerPlugInDriverRef drv)
{
    printf("=== PROPERTY SWEEP (objects 1..3 x scope x element 0/1/2) ===\n");
    for (AudioObjectID obj = 1; obj <= 3; obj++) {
        for (size_t s = 0; s < 3; s++) {
            for (UInt32 elem = 0; elem <= 2; elem++) {
                for (size_t i = 0; i < sizeof(kSelectors)/sizeof(kSelectors[0]); i++) {
                    AudioObjectPropertyAddress a = { kSelectors[i].sel, kScopes[s], elem };
                    if (!(*drv)->HasProperty(drv, obj, 0, &a)) continue;

                    UInt32 size1 = 0, size2 = 0;
                    OSStatus r1 = (*drv)->GetPropertyDataSize(drv, obj, 0, &a, 0, NULL, &size1);
                    OSStatus r2 = (*drv)->GetPropertyDataSize(drv, obj, 0, &a, 0, NULL, &size2);
                    if (r1 != noErr || r1 != r2 || size1 != size2) {
                        printf("  !! obj%u %s %s el%u: Size unstable (r=%d/%d, %u/%u)\n",
                               obj, kSelectors[i].name, kScopeNames[s], elem, (int)r1, (int)r2, size1, size2);
                        gProblems++;
                        continue;
                    }
                    UInt8 buf1[512] = {0}, buf2[512] = {0};
                    UInt32 out1 = 0, out2 = 0;
                    UInt32 cap = size1 > 512 ? 512 : size1;
                    OSStatus d1 = (*drv)->GetPropertyData(drv, obj, 0, &a, 0, NULL, cap, &out1, buf1);
                    OSStatus d2 = (*drv)->GetPropertyData(drv, obj, 0, &a, 0, NULL, cap, &out2, buf2);
                    if (d1 != noErr || d2 != noErr) {
                        printf("  !! obj%u %s %s el%u: Data error (%d/%d) but HasProperty=yes\n",
                               obj, kSelectors[i].name, kScopeNames[s], elem, (int)d1, (int)d2);
                        gProblems++;
                        continue;
                    }
                    if (out1 != size1) {
                        printf("  !! obj%u %s %s el%u: Size=%u but Data outSize=%u (MISMATCH -> coreaudiod storm)\n",
                               obj, kSelectors[i].name, kScopeNames[s], elem, size1, out1);
                        gProblems++;
                    }
                    if (out1 != out2 || memcmp(buf1, buf2, out1 < 512 ? out1 : 512) != 0) {
                        printf("  !! obj%u %s %s el%u: data unstable across calls\n",
                               obj, kSelectors[i].name, kScopeNames[s], elem);
                        gProblems++;
                    }
                    if (elem != 0) {
                        printf("  !! obj%u %s %s: answers element=%u (must be Main=0 only -> enumeration storm)\n",
                               obj, kSelectors[i].name, kScopeNames[s], elem);
                        gProblems++;
                    }
                }
            }
        }
    }
    printf("Property sweep: %s (%d problems)\n\n", gProblems ? "PROBLEMS FOUND" : "OK", gProblems);
}

// ---------- IO simulation (needs `line6-daemon daemon --sine`) ----------
static void iosim(AudioServerPlugInDriverRef drv, double seconds)
{
    printf("=== IO SIMULATION (%0.1f s, 512-frame HAL cycle) ===\n", seconds);
    struct mach_timebase_info tb; mach_timebase_info(&tb);
    double ticksPerSec = 1e9 * (double)tb.denom / (double)tb.numer;
    double ticksPerFrame = ticksPerSec / 48000.0;

    OSStatus r = (*drv)->StartIO(drv, 2, 0);
    if (r != noErr) { printf("StartIO error %d\n", (int)r); gProblems++; return; }

    const UInt32 B = 512;
    size_t maxFrames = (size_t)(seconds * 48000.0) + 4096;
    float* cap = calloc(maxFrames, sizeof(float));
    size_t got = 0;

    Float64 zSample = 0; UInt64 zHost = 0, zSeed = 0, lastSeed = 0;
    int seedChanges = -1; // ignore the first (initial) seed

    // Even pacing on the absolute clock, like coreaudiod: one DoIO per B frames
    // of host time. Residual resyncs are real host<->USB drift.
    double st = 0;
    UInt64 cycle = 0;
    UInt64 start = mach_absolute_time();
    double tickPerBuf = ticksPerFrame * (double)B;
    UInt64 nextCall = start + (UInt64)tickPerBuf;
    while ((mach_absolute_time() - start) < (UInt64)(seconds * ticksPerSec) && got + B < maxFrames) {
        UInt64 now = mach_absolute_time();
        if (now < nextCall) { usleep(200); continue; }
        nextCall += (UInt64)tickPerBuf;

        r = (*drv)->GetZeroTimeStamp(drv, 2, 0, &zSample, &zHost, &zSeed);
        if (r != noErr) { printf("GetZeroTimeStamp error %d\n", (int)r); gProblems++; break; }
        if (zSeed != lastSeed) { if (seedChanges >= 0) printf("  ! seed %llu->%llu\n", (unsigned long long)lastSeed, (unsigned long long)zSeed); seedChanges++; lastSeed = zSeed; }

        float buf[512 * LINE6_MAX_CHANNELS];
        AudioServerPlugInIOCycleInfo ci; memset(&ci, 0, sizeof(ci));
        ci.mIOCycleCounter = ++cycle;
        ci.mNominalIOBufferFrameSize = B;
        ci.mInputTime.mSampleTime = st; st += (double)B;
        ci.mInputTime.mHostTime = now;
        r = (*drv)->DoIOOperation(drv, 2, 3, 0, kAudioServerPlugInIOOperationReadInput, B, &ci, buf, NULL);
        if (r != noErr) { printf("DoIO error %d\n", (int)r); gProblems++; break; }
        for (UInt32 i = 0; i < B && got < maxFrames; i++) cap[got++] = buf[i * gChannels]; // channel 0
    }
    (*drv)->StopIO(drv, 2, 0);

    // Analyse continuity of the 440 Hz tone on channel 0.
    size_t zeros = 0, clicks = 0; double peak = 0;
    ssize_t firstZero = -1, lastZero = -1;
    for (size_t i = 0; i < got; i++) {
        double v = fabs(cap[i]); if (v > peak) peak = v;
        if (v < 1e-9) { zeros++; if (firstZero < 0) firstZero = (ssize_t)i; lastZero = (ssize_t)i; }
    }
    if (zeros)
        printf("zero samples: first @%zd, last @%zd (of %zu)\n", firstZero, lastZero, got);
    double maxStep = peak * 2.0 * M_PI * 440.0 / 48000.0 * 2.5;
    for (size_t i = 1; i < got; i++)
        if (fabs((double)cap[i] - (double)cap[i-1]) > maxStep) clicks++;
    size_t zc = 0; ssize_t firstZc = -1, lastZc = -1;
    for (size_t i = 1; i < got; i++)
        if (cap[i-1] < 0 && cap[i] >= 0) { zc++; if (firstZc < 0) firstZc = (ssize_t)i; lastZc = (ssize_t)i; }
    double freq = (zc > 2 && lastZc > firstZc) ? 48000.0 * (double)(zc - 1) / (double)(lastZc - firstZc) : 0;

    printf("frames: %zu, peak=%.3f, zeros: %zu (%.1f%%), discontinuities: %zu, freq=%.2f Hz, seed jumps: %d\n",
           got, peak, zeros, 100.0 * zeros / (got ? got : 1), clicks, freq, seedChanges > 0 ? seedChanges : 0);

    int fail = 0;
    if (got < (size_t)(seconds * 48000.0 * 0.5)) { printf("  !! too little data\n"); fail++; }
    if (peak < 0.1) { printf("  !! no signal (is `daemon --sine` running?)\n"); fail++; }
    if (clicks > 2) { printf("  !! stream discontinuities (%zu)\n", clicks); fail++; } // 1-2 startup resyncs are ok
    if (got && 100.0 * zeros / got > 2.0) { printf("  !! too many zeros (underruns)\n"); fail++; }
    if (freq > 1 && fabs(freq - 440.0) > 1.0) { printf("  !! frequency drifted\n"); fail++; }
    gProblems += fail;
    printf("IO simulation: %s\n\n", fail ? "FAIL" : "OK");
    free(cap);
}

int main(int argc, char** argv)
{
    const char* path = "Line6.driver/Contents/MacOS/Line6HAL";
    double seconds = 6.0;
    int sweepOnly = 0; // --sweep-only: skip the IO simulation (needs no device)
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sweep-only") == 0) { sweepOnly = 1; continue; }
        if (positional++ == 0) path = argv[i]; else seconds = atof(argv[i]);
    }

    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void* (*factory)(CFAllocatorRef, CFUUIDRef) =
        (void* (*)(CFAllocatorRef, CFUUIDRef))dlsym(h, "Line6_Create");
    if (!factory) { fprintf(stderr, "dlsym Line6_Create: %s\n", dlerror()); return 1; }

    AudioServerPlugInDriverRef drv =
        (AudioServerPlugInDriverRef)factory(NULL, kAudioServerPlugInTypeUUID);
    if (!drv) { fprintf(stderr, "factory returned NULL\n"); return 1; }

    peek_attach();
    OSStatus r = (*drv)->Initialize(drv, &gHost);
    printf("Initialize: %d\n\n", (int)r);
    if (r != noErr) return 1;

    sweep(drv);
    if (!sweepOnly) iosim(drv, seconds);

    printf("PropertiesChanged from plugin: %u\n", gNotifications);
    printf("=== RESULT: %s (%d problems) ===\n", gProblems ? "FAIL" : "PASS", gProblems);
    return gProblems ? 2 : 0;
}
