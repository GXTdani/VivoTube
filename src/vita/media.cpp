#include "media.h"

#include <psp2/audioout.h>
#include <psp2/kernel/sysmem.h>

#include <malloc.h>
#include <cstring>

#include "../core/util.h"

namespace vt {

namespace {
constexpr uint32_t kAlreadyInit = 0x807F0003;  // SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED
size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

// ---------------------------------------------------------------- H.264

bool AvcDecoder::open(int pitch, int alignedHeight) {
    close();
    pitch_ = pitch;
    height_ = alignedHeight;
    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = (uint32_t)pitch;
    init.vertical = (uint32_t)alignedHeight;
    init.numOfRefFrames = 5;
    init.numOfStreams = 1;
    int rc = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (rc < 0) {
        VT_LOG("avcdec: sceVideodecInitLibrary failed 0x%08X", (unsigned)rc);
        return false;
    }
    libInit_ = true;

    SceAvcdecQueryDecoderInfo q;
    memset(&q, 0, sizeof(q));
    q.horizontal = init.horizontal;
    q.vertical = init.vertical;
    q.numOfRefFrames = init.numOfRefFrames;
    SceAvcdecDecoderInfo di;
    memset(&di, 0, sizeof(di));
    rc = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
    if (rc < 0) {
        VT_LOG("avcdec: QueryDecoderMemSize failed 0x%08X", (unsigned)rc);
        close();
        return false;
    }
    size_t sz = align_up(di.frameMemSize, 1024 * 1024);
    memblock_ = sceKernelAllocMemBlock("vt_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, nullptr);
    if (memblock_ < 0)
        memblock_ = sceKernelAllocMemBlock("vt_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, align_up(sz, 256 * 1024),
                                          nullptr);
    if (memblock_ < 0) {
        VT_LOG("avcdec: cannot allocate %u bytes of decoder memory", (unsigned)sz);
        close();
        return false;
    }
    memset(&ctrl_, 0, sizeof(ctrl_));
    sceKernelGetMemBlockBase(memblock_, &ctrl_.frameBuf.pBuf);
    ctrl_.frameBuf.size = (uint32_t)sz;
    rc = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &ctrl_, &q);
    if (rc < 0) {
        VT_LOG("avcdec: CreateDecoder %dx%d failed 0x%08X", pitch, alignedHeight, (unsigned)rc);
        close();
        return false;
    }
    open_ = true;
    VT_LOG("avcdec: opened %dx%d (%u KB)", pitch, alignedHeight, (unsigned)(sz / 1024));
    return true;
}

void AvcDecoder::close() {
    if (open_) sceAvcdecDeleteDecoder(&ctrl_);
    open_ = false;
    if (memblock_ >= 0) sceKernelFreeMemBlock(memblock_);
    memblock_ = -1;
    if (libInit_) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    libInit_ = false;
}

int AvcDecoder::decode(const uint8_t* data, size_t size, int64_t pts, int64_t dts, void* out, int64_t* outPts) {
    if (!open_) return -1;
    SceAvcdecAu au;
    memset(&au, 0, sizeof(au));
    au.pts.upper = (uint32_t)((uint64_t)pts >> 32);
    au.pts.lower = (uint32_t)((uint64_t)pts & 0xFFFFFFFF);
    au.dts.upper = (uint32_t)((uint64_t)dts >> 32);
    au.dts.lower = (uint32_t)((uint64_t)dts & 0xFFFFFFFF);
    au.es.pBuf = (void*)data;
    au.es.size = (uint32_t)size;

    SceAvcdecPicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.size = sizeof(pic);
    pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER;
    pic.frame.framePitch = (uint32_t)pitch_;
    pic.frame.frameWidth = (uint32_t)pitch_;
    pic.frame.frameHeight = (uint32_t)height_;
    pic.frame.pPicture[0] = out;
    pic.frame.pPicture[1] = (uint8_t*)out + (size_t)pitch_ * height_;
    SceAvcdecPicture* pp = &pic;
    SceAvcdecArrayPicture arr;
    memset(&arr, 0, sizeof(arr));
    arr.numOfElm = 1;
    arr.pPicture = &pp;
    int rc = sceAvcdecDecode(&ctrl_, &au, &arr);
    if (rc < 0) return rc;
    if (arr.numOfOutput == 0) return 0;
    *outPts = (int64_t)(((uint64_t)pic.info.pts.upper << 32) | pic.info.pts.lower);
    return 1;
}

// ---------------------------------------------------------------- AAC

bool AacDecoder::open(const Fmp4Init& init) {
    close();
    init_ = init;
    SceAudiodecInitParam ip;
    memset(&ip, 0, sizeof(ip));
    ip.size = sizeof(ip.aac);
    ip.aac.size = sizeof(ip.aac);
    ip.aac.totalStreams = 1;
    int rc = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ip);
    if (rc < 0 && (uint32_t)rc != kAlreadyInit) {
        VT_LOG("aacdec: InitLibrary failed 0x%08X", (unsigned)rc);
        return false;
    }
    libInit_ = rc >= 0;
    memset(&info_, 0, sizeof(info_));
    info_.aac.size = sizeof(info_.aac);
    info_.aac.isAdts = 1;
    info_.aac.ch = (SceUInt32)(init.channels > 0 ? init.channels : 2);
    info_.aac.samplingRate = (SceUInt32)init.sampleRate;
    info_.aac.isSbr = init.sbr ? 1 : 0;
    memset(&ctrl_, 0, sizeof(ctrl_));
    ctrl_.size = sizeof(ctrl_);
    ctrl_.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    ctrl_.pInfo = &info_;
    rc = sceAudiodecCreateDecoder(&ctrl_, SCE_AUDIODEC_TYPE_AAC);
    if (rc < 0) {
        VT_LOG("aacdec: CreateDecoder (%d Hz, %d ch, sbr=%d) failed 0x%08X", init.sampleRate, init.channels, init.sbr,
               (unsigned)rc);
        close();
        return false;
    }
    // The decoder may read up to maxEsSize bytes past pEs, so size the buffer generously.
    esCap_ = align_up((ctrl_.maxEsSize > 0 ? ctrl_.maxEsSize : SCE_AUDIODEC_AAC_MAX_ES_SIZE) + 0x200, 0x100);
    size_t pcmCap = align_up(ctrl_.maxPcmSize > 0 ? ctrl_.maxPcmSize : SCE_AUDIODEC_AAC_MAX_SAMPLES * 2 * 2 * 2, 0x100);
    es_ = (uint8_t*)memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, esCap_);
    pcm_ = (int16_t*)memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, pcmCap);
    if (!es_ || !pcm_) {
        close();
        return false;
    }
    memset(es_, 0, esCap_);
    ctrl_.maxPcmSize = (SceUInt32)pcmCap;
    outRate_ = init.sbr ? init.sampleRate * 2 : init.sampleRate;
    samples_ = init.sbr ? 2048 : 1024;
    open_ = true;
    VT_LOG("aacdec: opened aot=%d %d Hz ch=%d sbr=%d (out %d Hz, %d samples)", init.audioObjectType, init.sampleRate,
           init.channels, init.sbr, outRate_, samples_);
    return true;
}

void AacDecoder::close() {
    if (open_) sceAudiodecDeleteDecoder(&ctrl_);
    open_ = false;
    if (libInit_) sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
    libInit_ = false;
    free(es_);
    free(pcm_);
    es_ = nullptr;
    pcm_ = nullptr;
}

int AacDecoder::decode(const uint8_t* frame, size_t size, int16_t* out) {
    if (!open_ || size + 7 > esCap_) return -1;
    make_adts_header(init_, size, es_);
    memcpy(es_ + 7, frame, size);
    memset(es_ + 7 + size, 0, esCap_ - 7 - size < 64 ? esCap_ - 7 - size : 64);
    ctrl_.pEs = es_;
    ctrl_.inputEsSize = (SceUInt32)(size + 7);
    ctrl_.pPcm = pcm_;
    ctrl_.outputPcmSize = 0;
    int rc = sceAudiodecDecode(&ctrl_);
    if (rc < 0) return rc;
    int ch = init_.channels == 1 ? 1 : 2;
    int samples = (int)(ctrl_.outputPcmSize / (2 * ch));
    if (samples > samples_) samples = samples_;
    if (ch == 2) {
        memcpy(out, pcm_, (size_t)samples * 4);
    } else {
        for (int i = 0; i < samples; i++) out[2 * i] = out[2 * i + 1] = pcm_[i];
    }
    return samples;
}

// ---------------------------------------------------------------- output

bool AudioPort::open(int sampleRate, int grain) {
    close();
    port_ = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, sampleRate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port_ < 0) {
        VT_LOG("audio: OpenPort(%d Hz, grain %d) failed 0x%08X", sampleRate, grain, (unsigned)port_);
        port_ = -1;
        return false;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port_, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    grain_ = grain;
    rate_ = sampleRate;
    return true;
}

void AudioPort::close() {
    if (port_ >= 0) {
        sceAudioOutOutput(port_, nullptr);
        sceAudioOutReleasePort(port_);
    }
    port_ = -1;
}

void AudioPort::output(const int16_t* stereo) {
    if (port_ >= 0) sceAudioOutOutput(port_, stereo);
}

}  // namespace vt
