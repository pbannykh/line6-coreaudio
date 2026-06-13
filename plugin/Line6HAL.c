//
//  Line6HAL.c
//  CoreAudio HAL AudioServerPlugin for Line 6 USB audio devices.
//
//  Publishes one input device and feeds it audio from the shared-memory ring
//  filled by the daemon (../shared, ../daemon). Device-agnostic: the channel
//  count and model name come from the shared-memory header.
//
//  Two invariants keep coreaudiod stable (haltest checks both): answer only the
//  Main element, and keep GetPropertyDataSize consistent with GetPropertyData.
//

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>      // kAudioDevicePropertyStreamConfiguration
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <os/log.h>

#include "line6_shm.h"

// Diagnostics: log show --predicate 'subsystem == "org.line6"' --last 5m
static os_log_t gLog;

// ----------------------------------------------------------------------------
#pragma mark Constants
// ----------------------------------------------------------------------------

enum {
    kObjectID_PlugIn       = kAudioObjectPlugInObject, // 1
    kObjectID_Device       = 2,
    kObjectID_Stream_Input = 3,
};

#define kDevice_UID        "Line6_Device_UID"
#define kDevice_ModelUID   "Line6_Model_UID"
#define kManufacturer_Name "Line 6 (community driver)"

#define kSampleRate        48000.0
#define kBytesPerSample    4              // Float32

// Ring period reported as the zero-timestamp period (frames per timeline tick).
#define kDevice_RingPeriod 8192
// Reported safety offset and the default read-cursor depth (frames). The actual
// depth is the larger of this and the daemon's latency_frames from shared memory.
#define kSafetyOffset      256
#define kDefaultMargin     3072           // ~64 ms at 48 kHz

// ----------------------------------------------------------------------------
#pragma mark Globals
// ----------------------------------------------------------------------------

static pthread_mutex_t          gMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32                   gRefCount = 0;
static AudioServerPlugInHostRef gHost = NULL;

static UInt64  gIO_Running = 0;          // number of active StartIO clients
static Float64 gHostTicksPerFrame = 0.0; // mach ticks per audio frame

// Host-clock timeline anchor (see GetZeroTimeStamp).
static UInt64  gAnchorHostTime = 0;
static UInt64  gNumberTimeStamps = 0;

// Shared memory (read-only consumer; the daemon fills it).
static line6_shm_t* gShm = NULL;

// Read cursor. Advanced exactly once per IO cycle (keyed on mIOCycleCounter) so
// multiple clients in one cycle read the same contiguous position.
static UInt64  gReadFrame = 0;
static UInt64  gCycleReadFrame = 0;
static UInt64  gLastCycle = ~0ULL;

// Device presence: 1 = listed, 0 = removed (device unplugged / daemon stopped).
static _Atomic int gDevicePresent = 1;
static _Atomic int gMonitorRun = 1;
static pthread_t   gMonitorThread;

// ----------------------------------------------------------------------------
#pragma mark Shared memory
// ----------------------------------------------------------------------------

static void AttachShm(void)
{
    if (gShm != NULL) return;            // fast path; gShm is written once
    pthread_mutex_lock(&gMutex);          // called from IO and monitor threads
    if (gShm != NULL) { pthread_mutex_unlock(&gMutex); return; }
    int fd = shm_open(LINE6_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) { pthread_mutex_unlock(&gMutex); return; } // daemon not running
    void* p = mmap(NULL, LINE6_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        os_log(gLog, "AttachShm: mmap failed errno=%d", errno);
        pthread_mutex_unlock(&gMutex);
        return;
    }
    line6_shm_t* shm = (line6_shm_t*)p;
    if (shm->magic != LINE6_SHM_MAGIC || shm->version != LINE6_SHM_VERSION) {
        os_log(gLog, "AttachShm: bad header magic=0x%x ver=%u", shm->magic, shm->version);
        munmap(p, LINE6_SHM_SIZE);
        pthread_mutex_unlock(&gMutex);
        return;
    }
    gShm = shm;
    pthread_mutex_unlock(&gMutex);
    os_log(gLog, "AttachShm: ok");
}

// Capture channel count and frame size, from the connected device (via shm).
static UInt32 DevChannels(void)
{
    UInt32 c = gShm ? gShm->channels : LINE6_MAX_CHANNELS;
    if (c == 0 || c > LINE6_MAX_CHANNELS) c = LINE6_MAX_CHANNELS;
    return c;
}

// Model name as a +1 CFString the caller releases. The string is cached and
// only rebuilt when the connected model changes, so repeated queries return the
// same object (stable identity), and recreated on hot-swap.
static CFStringRef gCachedName = NULL;
static char        gCachedNameStr[LINE6_NAME_LEN] = {0};

static CFStringRef DeviceNameCopy(void)
{
    char buf[LINE6_NAME_LEN];
    if (gShm) {
        memcpy(buf, gShm->name, LINE6_NAME_LEN);
        buf[LINE6_NAME_LEN - 1] = 0;
    } else {
        buf[0] = 0;
    }
    if (buf[0] == 0) strcpy(buf, "Line 6");

    pthread_mutex_lock(&gMutex);
    if (gCachedName == NULL || strcmp(buf, gCachedNameStr) != 0) {
        if (gCachedName) CFRelease(gCachedName);
        gCachedName = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
        strncpy(gCachedNameStr, buf, LINE6_NAME_LEN - 1);
    }
    CFStringRef result = gCachedName;
    CFRetain(result); // caller releases its reference
    pthread_mutex_unlock(&gMutex);
    return result;
}

// ----------------------------------------------------------------------------
#pragma mark Presence monitor
// ----------------------------------------------------------------------------
//
// write_frames grows only while a device is connected and capturing. The monitor
// polls it; if it stops growing the device is removed from the device list (and
// re-added when it grows again), so unplugging behaves like any USB interface.

#define PRESENCE_POLL_US   500000   // 0.5 s
#define PRESENCE_STALL_MAX 4        // 2 s with no growth -> absent

static void NotifyDeviceList(void)
{
    if (gHost == NULL) return;
    AudioObjectPropertyAddress addr = {
        kAudioPlugInPropertyDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    gHost->PropertiesChanged(gHost, kObjectID_PlugIn, 1, &addr);
}

static void* PresenceMonitor(void* arg)
{
    (void)arg;
    UInt64 lastWf = 0;
    int haveLast = 0, stall = 0;
    while (atomic_load_explicit(&gMonitorRun, memory_order_relaxed)) {
        usleep(PRESENCE_POLL_US);
        AttachShm();
        int present;
        if (gShm == NULL) {
            present = 0;
            haveLast = 0;
        } else {
            UInt64 wf = atomic_load_explicit(&gShm->write_frames, memory_order_acquire);
            if (!haveLast || wf != lastWf) {
                lastWf = wf; haveLast = 1; stall = 0; present = 1;
            } else {
                if (stall < PRESENCE_STALL_MAX) stall++;
                present = (stall < PRESENCE_STALL_MAX);
            }
        }
        int prev = atomic_exchange_explicit(&gDevicePresent, present, memory_order_acq_rel);
        if (prev != present) {
            os_log(gLog, "presence: device %{public}s", present ? "appeared" : "gone");
            NotifyDeviceList();   // not holding the lock: callback may re-enter
        }
    }
    return NULL;
}

// ----------------------------------------------------------------------------
#pragma mark Interface vtable and factory
// ----------------------------------------------------------------------------

static HRESULT  Line6_QueryInterface(void*, REFIID, LPVOID*);
static ULONG    Line6_AddRef(void*);
static ULONG    Line6_Release(void*);
static OSStatus Line6_Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus Line6_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus Line6_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus Line6_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus Line6_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus Line6_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus Line6_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean  Line6_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus Line6_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus Line6_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus Line6_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus Line6_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus Line6_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus Line6_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus Line6_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus Line6_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus Line6_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus Line6_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus Line6_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    Line6_QueryInterface,
    Line6_AddRef,
    Line6_Release,
    Line6_Initialize,
    Line6_CreateDevice,
    Line6_DestroyDevice,
    Line6_AddDeviceClient,
    Line6_RemoveDeviceClient,
    Line6_PerformDeviceConfigurationChange,
    Line6_AbortDeviceConfigurationChange,
    Line6_HasProperty,
    Line6_IsPropertySettable,
    Line6_GetPropertyDataSize,
    Line6_GetPropertyData,
    Line6_SetPropertyData,
    Line6_StartIO,
    Line6_StopIO,
    Line6_GetZeroTimeStamp,
    Line6_WillDoIOOperation,
    Line6_BeginIOOperation,
    Line6_DoIOOperation,
    Line6_EndIOOperation
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// Factory named in Info.plist (CFPlugInFactories).
void* Line6_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
void* Line6_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return gDriverRef;
    }
    return NULL;
}

// ----------------------------------------------------------------------------
#pragma mark IUnknown
// ----------------------------------------------------------------------------

static HRESULT Line6_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gDriverRef || outInterface == NULL) return E_INVALIDARG;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gMutex);
        ++gRefCount;
        pthread_mutex_unlock(&gMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(req);
    return result;
}

static ULONG Line6_AddRef(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gMutex);
    ULONG r = ++gRefCount;
    pthread_mutex_unlock(&gMutex);
    return r;
}

static ULONG Line6_Release(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gMutex);
    ULONG r = (gRefCount > 0) ? --gRefCount : 0;
    pthread_mutex_unlock(&gMutex);
    return r;
}

// ----------------------------------------------------------------------------
#pragma mark Lifecycle
// ----------------------------------------------------------------------------

static OSStatus Line6_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    gLog = os_log_create("org.line6", "hal");
    os_log(gLog, "Initialize");

    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    Float64 hostClockHz = (1.0e9 * (Float64)tb.denom) / (Float64)tb.numer;
    gHostTicksPerFrame = hostClockHz / kSampleRate;

    AttachShm();

    atomic_store_explicit(&gMonitorRun, 1, memory_order_relaxed);
    if (pthread_create(&gMonitorThread, NULL, PresenceMonitor, NULL) != 0) {
        os_log(gLog, "Initialize: failed to start presence monitor");
        gMonitorThread = 0;
    }
    return noErr;
}

// This plugin owns one fixed device; it does not create devices on request.
static OSStatus Line6_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* c, AudioObjectID* out)
{ (void)d;(void)desc;(void)c;(void)out; return kAudioHardwareUnsupportedOperationError; }
static OSStatus Line6_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{ (void)d;(void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus Line6_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo* c)
{ (void)d;(void)o;(void)c; return noErr; }
static OSStatus Line6_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo* c)
{ (void)d;(void)o;(void)c; return noErr; }
static OSStatus Line6_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void* i)
{ (void)d;(void)o;(void)a;(void)i; return noErr; }
static OSStatus Line6_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void* i)
{ (void)d;(void)o;(void)a;(void)i; return noErr; }

// ----------------------------------------------------------------------------
#pragma mark Property helpers
// ----------------------------------------------------------------------------

// 48 kHz / Float32 interleaved format for a given channel count.
static void FillASBD(AudioStreamBasicDescription* f, UInt32 channels)
{
    f->mSampleRate       = kSampleRate;
    f->mFormatID         = kAudioFormatLinearPCM;
    f->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f->mBytesPerPacket   = channels * kBytesPerSample;
    f->mFramesPerPacket  = 1;
    f->mBytesPerFrame    = channels * kBytesPerSample;
    f->mChannelsPerFrame = channels;
    f->mBitsPerChannel   = 32;
    f->mReserved         = 0;
}

// The device's streams for a scope. Input only (no playback).
static UInt32 DeviceStreamList(AudioObjectPropertyScope scope, AudioObjectID* out)
{
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
        out[n++] = kObjectID_Stream_Input;
    return n;
}

// ----------------------------------------------------------------------------
#pragma mark HasProperty / IsPropertySettable
// ----------------------------------------------------------------------------

static Boolean Line6_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClient, const AudioObjectPropertyAddress* inAddr)
{
    UInt32 size = 0;
    return Line6_GetPropertyDataSize(inDriver, inObjectID, inClient, inAddr, 0, NULL, &size) == noErr;
}

static OSStatus Line6_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClient, const AudioObjectPropertyAddress* inAddr, Boolean* outSettable)
{
    UInt32 size = 0;
    if (Line6_GetPropertyDataSize(inDriver, inObjectID, inClient, inAddr, 0, NULL, &size) != noErr)
        return kAudioHardwareUnknownPropertyError;
    *outSettable = false; // everything is read-only (fixed format)
    return noErr;
}

static OSStatus Line6_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t c, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32 ds, const void* dt)
{ (void)d;(void)o;(void)c;(void)a;(void)qs;(void)q;(void)ds;(void)dt; return kAudioHardwareUnknownPropertyError; }

// ----------------------------------------------------------------------------
#pragma mark GetPropertyDataSize
// ----------------------------------------------------------------------------

static OSStatus Line6_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClient, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32* outSize)
{
    (void)inDriver;(void)inClient;(void)qs;(void)q;

    if (a->mElement != kAudioObjectPropertyElementMain)
        return kAudioHardwareUnknownPropertyError;

    if (inObjectID == kObjectID_PlugIn) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:           *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyManufacturer:    *outSize = sizeof(CFStringRef); return noErr;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: // 0 devices when absent
                *outSize = atomic_load_explicit(&gDevicePresent, memory_order_acquire)
                    ? sizeof(AudioObjectID) : 0;
                return noErr;
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyClockDeviceList: *outSize = 0; return noErr;
            case kAudioPlugInPropertyTranslateUIDToDevice: *outSize = sizeof(AudioObjectID); return noErr;
            case kAudioPlugInPropertyResourceBundle:  *outSize = sizeof(CFStringRef); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (inObjectID == kObjectID_Device) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:           *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:    *outSize = sizeof(CFStringRef); return noErr;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                AudioObjectID tmp[2];
                *outSize = DeviceStreamList(a->mScope, tmp) * sizeof(AudioObjectID);
                return noErr;
            }
            case kAudioObjectPropertyControlList:     *outSize = 0; return noErr;
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:        *outSize = sizeof(CFStringRef); return noErr;
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyZeroTimeStampPeriod: *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyNominalSampleRate: *outSize = sizeof(Float64); return noErr;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outSize = sizeof(AudioValueRange); return noErr;
            case kAudioDevicePropertyRelatedDevices:  *outSize = sizeof(AudioObjectID); return noErr;
            case kAudioDevicePropertyStreamConfiguration:
                // Input only: no buffers in the output scope (must match GetPropertyData).
                *outSize = (a->mScope == kAudioObjectPropertyScopeOutput)
                    ? (UInt32)offsetof(AudioBufferList, mBuffers)
                    : (UInt32)(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer));
                return noErr;
            case kAudioDevicePropertyPreferredChannelsForStereo: *outSize = 2 * sizeof(UInt32); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (inObjectID == kObjectID_Stream_Input) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:           *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwnedObjects:    *outSize = 0; return noErr;
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:         *outSize = sizeof(UInt32); return noErr;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:  *outSize = sizeof(AudioStreamBasicDescription); return noErr;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: *outSize = sizeof(AudioStreamRangedDescription); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;
    }

    return kAudioHardwareBadObjectError;
}

// ----------------------------------------------------------------------------
#pragma mark GetPropertyData
// ----------------------------------------------------------------------------

static OSStatus Line6_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClient, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32 inSize, UInt32* outSize, void* outData)
{
    (void)inDriver;(void)inClient;(void)qs;(void)q;

    if (a->mElement != kAudioObjectPropertyElementMain)
        return kAudioHardwareUnknownPropertyError;

    if (inObjectID == kObjectID_PlugIn) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:  *(AudioClassID*)outData = kAudioObjectClassID; *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyClass:      *(AudioClassID*)outData = kAudioPlugInClassID;  *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:      *(AudioObjectID*)outData = kAudioObjectUnknown; *outSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = CFSTR(kManufacturer_Name); *outSize = sizeof(CFStringRef); return noErr;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
                // Empty list when the device is absent (matches GetPropertyDataSize).
                if (atomic_load_explicit(&gDevicePresent, memory_order_acquire)
                    && inSize >= sizeof(AudioObjectID)) {
                    *(AudioObjectID*)outData = kObjectID_Device; *outSize = sizeof(AudioObjectID);
                } else *outSize = 0;
                return noErr;
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyClockDeviceList: *outSize = 0; return noErr;
            case kAudioPlugInPropertyTranslateUIDToDevice: {
                CFStringRef uid = (qs >= sizeof(CFStringRef) && q) ? *(CFStringRef*)q : NULL;
                Boolean ok = atomic_load_explicit(&gDevicePresent, memory_order_acquire)
                    && uid && CFEqual(uid, CFSTR(kDevice_UID));
                *(AudioObjectID*)outData = ok ? (AudioObjectID)kObjectID_Device : (AudioObjectID)kAudioObjectUnknown;
                *outSize = sizeof(AudioObjectID); return noErr;
            }
            case kAudioPlugInPropertyResourceBundle:
                *(CFStringRef*)outData = CFSTR(""); *outSize = sizeof(CFStringRef); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (inObjectID == kObjectID_Device) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: *(AudioClassID*)outData = kAudioObjectClassID; *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyClass:     *(AudioClassID*)outData = kAudioDeviceClassID; *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:     *(AudioObjectID*)outData = kObjectID_PlugIn;   *outSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyName:         *(CFStringRef*)outData = DeviceNameCopy();          *outSize = sizeof(CFStringRef); return noErr;
            case kAudioObjectPropertyManufacturer: *(CFStringRef*)outData = CFSTR(kManufacturer_Name); *outSize = sizeof(CFStringRef); return noErr;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                AudioObjectID list[2];
                UInt32 n = DeviceStreamList(a->mScope, list);
                UInt32 fit = inSize / sizeof(AudioObjectID);
                if (fit > n) fit = n;
                memcpy(outData, list, fit * sizeof(AudioObjectID));
                *outSize = fit * sizeof(AudioObjectID);
                return noErr;
            }
            case kAudioObjectPropertyControlList: *outSize = 0; return noErr;
            case kAudioDevicePropertyDeviceUID:  *(CFStringRef*)outData = CFSTR(kDevice_UID);  *outSize = sizeof(CFStringRef); return noErr;
            case kAudioDevicePropertyModelUID:   *(CFStringRef*)outData = CFSTR(kDevice_ModelUID); *outSize = sizeof(CFStringRef); return noErr;
            case kAudioDevicePropertyTransportType: *(UInt32*)outData = kAudioDeviceTransportTypeUSB; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyClockDomain:   *(UInt32*)outData = 0; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyDeviceIsAlive: *(UInt32*)outData = 1; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyDeviceIsRunning: *(UInt32*)outData = (gIO_Running > 0) ? 1 : 0; *outSize = sizeof(UInt32); return noErr;
            // Selectable as the default input.
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: *(UInt32*)outData = 1; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: *(UInt32*)outData = 0; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyLatency:       *(UInt32*)outData = 0; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertySafetyOffset:  *(UInt32*)outData = kSafetyOffset; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyIsHidden:      *(UInt32*)outData = 0; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyZeroTimeStampPeriod: *(UInt32*)outData = kDevice_RingPeriod; *outSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyNominalSampleRate: *(Float64*)outData = kSampleRate; *outSize = sizeof(Float64); return noErr;
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange* r = (AudioValueRange*)outData;
                r->mMinimum = kSampleRate; r->mMaximum = kSampleRate; *outSize = sizeof(AudioValueRange); return noErr;
            }
            case kAudioDevicePropertyRelatedDevices:
                if (inSize >= sizeof(AudioObjectID)) { *(AudioObjectID*)outData = kObjectID_Device; *outSize = sizeof(AudioObjectID); }
                else *outSize = 0;
                return noErr;
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* bl = (AudioBufferList*)outData;
                if (a->mScope == kAudioObjectPropertyScopeOutput) {
                    bl->mNumberBuffers = 0;
                    *outSize = offsetof(AudioBufferList, mBuffers);
                    return noErr;
                }
                bl->mNumberBuffers = 1;
                bl->mBuffers[0].mNumberChannels = DevChannels();
                bl->mBuffers[0].mDataByteSize = 0;
                bl->mBuffers[0].mData = NULL;
                *outSize = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer);
                return noErr;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                UInt32* ch = (UInt32*)outData; ch[0] = 1; ch[1] = 2; *outSize = 2 * sizeof(UInt32); return noErr;
            }
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (inObjectID == kObjectID_Stream_Input) {
        UInt32 chans = DevChannels();
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: *(AudioClassID*)outData = kAudioObjectClassID; *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyClass:     *(AudioClassID*)outData = kAudioStreamClassID; *outSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:     *(AudioObjectID*)outData = kObjectID_Device;   *outSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyOwnedObjects: *outSize = 0; return noErr;
            case kAudioStreamPropertyIsActive:    *(UInt32*)outData = 1; *outSize = sizeof(UInt32); return noErr;
            case kAudioStreamPropertyDirection:   *(UInt32*)outData = 1; *outSize = sizeof(UInt32); return noErr; // input
            case kAudioStreamPropertyTerminalType: *(UInt32*)outData = kAudioStreamTerminalTypeUnknown; *outSize = sizeof(UInt32); return noErr;
            case kAudioStreamPropertyStartingChannel: *(UInt32*)outData = 1; *outSize = sizeof(UInt32); return noErr;
            case kAudioStreamPropertyLatency:     *(UInt32*)outData = 0; *outSize = sizeof(UInt32); return noErr;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                FillASBD((AudioStreamBasicDescription*)outData, chans); *outSize = sizeof(AudioStreamBasicDescription); return noErr;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription* r = (AudioStreamRangedDescription*)outData;
                FillASBD(&r->mFormat, chans);
                r->mSampleRateRange.mMinimum = kSampleRate;
                r->mSampleRateRange.mMaximum = kSampleRate;
                *outSize = sizeof(AudioStreamRangedDescription);
                return noErr;
            }
        }
        return kAudioHardwareUnknownPropertyError;
    }

    return kAudioHardwareBadObjectError;
}

// ----------------------------------------------------------------------------
#pragma mark IO
// ----------------------------------------------------------------------------

static OSStatus Line6_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    if (gIO_Running == 0) {
        AttachShm();
        gAnchorHostTime = mach_absolute_time();
        gNumberTimeStamps = 0;
        UInt64 w0 = gShm ? atomic_load_explicit(&gShm->write_frames, memory_order_acquire) : 0;
        gReadFrame = w0;       // the cursor re-anchors on the first DoIO
        gCycleReadFrame = w0;
        gLastCycle = ~0ULL;
    }
    ++gIO_Running;
    pthread_mutex_unlock(&gMutex);
    return noErr;
}

static OSStatus Line6_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    if (gIO_Running > 0) --gIO_Running;
    pthread_mutex_unlock(&gMutex);
    return noErr;
}

static OSStatus Line6_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    // Stable, monotone 48 kHz timeline on the host clock: each period boundary
    // lies on the line through the anchor, so the seed stays constant.
    UInt64 now = mach_absolute_time();
    Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kDevice_RingPeriod;
    UInt64 nextWrapTime = gAnchorHostTime + (UInt64)(((Float64)(gNumberTimeStamps + 1)) * ticksPerPeriod);
    if (now >= nextWrapTime) gNumberTimeStamps++;
    *outSampleTime = (Float64)(gNumberTimeStamps * kDevice_RingPeriod);
    *outHostTime   = gAnchorHostTime + (UInt64)((Float64)gNumberTimeStamps * ticksPerPeriod);
    *outSeed       = 1;
    pthread_mutex_unlock(&gMutex);
    return noErr;
}

static OSStatus Line6_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    (void)inDriver;(void)inDeviceID;(void)inClientID;
    Boolean willDo = (inOperationID == kAudioServerPlugInIOOperationReadInput);
    if (outWillDo) *outWillDo = willDo;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return noErr;
}

static OSStatus Line6_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 fs, const AudioServerPlugInIOCycleInfo* ci)
{ (void)d;(void)o;(void)c;(void)op;(void)fs;(void)ci; return noErr; }
static OSStatus Line6_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 fs, const AudioServerPlugInIOCycleInfo* ci)
{ (void)d;(void)o;(void)c;(void)op;(void)fs;(void)ci; return noErr; }

static OSStatus Line6_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceID, AudioObjectID inStreamID, UInt32 inClientID, UInt32 inOperationID, UInt32 inFrames, const AudioServerPlugInIOCycleInfo* ci, void* ioMainBuffer, void* ioSecondaryBuffer)
{
    (void)inDriver;(void)inDeviceID;(void)inStreamID;(void)inClientID;(void)ioSecondaryBuffer;
    if (inOperationID != kAudioServerPlugInIOOperationReadInput) return noErr;

    float* out = (float*)ioMainBuffer;
    if (out == NULL) return noErr;
    UInt32 ch = DevChannels();
    UInt32 frameBytes = ch * (UInt32)kBytesPerSample;

    if (gShm == NULL) {
        memset(out, 0, (size_t)inFrames * frameBytes);
        return noErr;
    }

    UInt64 w = atomic_load_explicit(&gShm->write_frames, memory_order_acquire);

    pthread_mutex_lock(&gMutex);
    // Advance the cursor once per IO cycle so several clients in one cycle read
    // the same position. Keep it behind the write head by at least the buffer.
    if (ci->mIOCycleCounter != gLastCycle) {
        gLastCycle = ci->mIOCycleCounter;
        UInt64 lat = (UInt64)inFrames * 2 + kSafetyOffset;
        UInt32 margin = gShm->latency_frames ? gShm->latency_frames : (UInt32)kDefaultMargin;
        if (lat < margin) lat = margin;
        if (gReadFrame + inFrames > w            // reached the write head (underrun)
            || gReadFrame > w                     // overran
            || (w - gReadFrame) > (LINE6_RING_FRAMES - inFrames)) { // fell behind (overrun)
            gReadFrame = (w > lat) ? (w - lat) : 0;
        }
        gCycleReadFrame = gReadFrame;
        gReadFrame += inFrames;
    }
    UInt64 rf = gCycleReadFrame;
    pthread_mutex_unlock(&gMutex);

    for (UInt32 i = 0; i < inFrames; ++i) {
        UInt64 src = rf + i;
        if (src < w) {
            const float* frame = &gShm->ring[(size_t)(src & LINE6_RING_MASK) * ch];
            memcpy(&out[(size_t)i * ch], frame, frameBytes);
        } else {
            memset(&out[(size_t)i * ch], 0, frameBytes); // underrun -> silence
        }
    }
    return noErr;
}
