/*************************************************************************
 * Copyright (C) [2019] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/

#include "easycodec/easy_encode.h"

#include <cn_codec_common.h>
#include <cn_jpeg_enc.h>
#include <cn_video_enc.h>
#include <cnrt.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "cxxutil/log.h"
#include "easyinfer/mlu_memory_op.h"
#include "format_info.h"

using std::to_string;
#define ALIGN(size, alignment) (((uint32_t)(size) + (alignment)-1) & ~((alignment)-1))

// cncodec add version macro since v1.6.0
#ifndef CNCODEC_VERSION
#define CNCODEC_VERSION 0
#endif

namespace edk {

// 2 MiB
static constexpr uint32_t g_buffer_size = 0x200000;

static cnvideoEncProfile ProfileCast(VideoProfile prof) {
  switch (prof) {
    case VideoProfile::H264_BASELINE:
      return CNVIDEOENC_PROFILE_H264_BASELINE;
    case VideoProfile::H264_MAIN:
      return CNVIDEOENC_PROFILE_H264_MAIN;
    case VideoProfile::H264_HIGH:
      return CNVIDEOENC_PROFILE_H264_HIGH;
    case VideoProfile::H264_HIGH_10:
      return CNVIDEOENC_PROFILE_H264_HIGH_10;
    case VideoProfile::H265_MAIN:
      return CNVIDEOENC_PROFILE_H265_MAIN;
    case VideoProfile::H265_MAIN_STILL:
      return CNVIDEOENC_PROFILE_H265_MAIN_STILL;
    case VideoProfile::H265_MAIN_INTRA:
      return CNVIDEOENC_PROFILE_H265_MAIN_INTRA;
    case VideoProfile::H265_MAIN_10:
      return CNVIDEOENC_PROFILE_H265_MAIN_10;
    default:
      return CNVIDEOENC_PROFILE_MAX;
  }
}

static cnvideoEncLevel LevelCast(VideoLevel level) {
  switch (level) {
    case VideoLevel::H264_1:
      return CNVIDEOENC_LEVEL_H264_1;
    case VideoLevel::H264_1B:
      return CNVIDEOENC_LEVEL_H264_1B;
    case VideoLevel::H264_11:
      return CNVIDEOENC_LEVEL_H264_11;
    case VideoLevel::H264_12:
      return CNVIDEOENC_LEVEL_H264_12;
    case VideoLevel::H264_13:
      return CNVIDEOENC_LEVEL_H264_13;
    case VideoLevel::H264_2:
      return CNVIDEOENC_LEVEL_H264_2;
    case VideoLevel::H264_21:
      return CNVIDEOENC_LEVEL_H264_21;
    case VideoLevel::H264_22:
      return CNVIDEOENC_LEVEL_H264_22;
    case VideoLevel::H264_3:
      return CNVIDEOENC_LEVEL_H264_3;
    case VideoLevel::H264_31:
      return CNVIDEOENC_LEVEL_H264_31;
    case VideoLevel::H264_32:
      return CNVIDEOENC_LEVEL_H264_32;
    case VideoLevel::H264_4:
      return CNVIDEOENC_LEVEL_H264_4;
    case VideoLevel::H264_41:
      return CNVIDEOENC_LEVEL_H264_41;
    case VideoLevel::H264_42:
      return CNVIDEOENC_LEVEL_H264_42;
    case VideoLevel::H264_5:
      return CNVIDEOENC_LEVEL_H264_5;
    case VideoLevel::H264_51:
      return CNVIDEOENC_LEVEL_H264_51;
    case VideoLevel::H265_MAIN_1:
      return CNVIDEOENC_LEVEL_H265_MAIN_1;
    case VideoLevel::H265_HIGH_1:
      return CNVIDEOENC_LEVEL_H265_HIGH_1;
    case VideoLevel::H265_MAIN_2:
      return CNVIDEOENC_LEVEL_H265_MAIN_2;
    case VideoLevel::H265_HIGH_2:
      return CNVIDEOENC_LEVEL_H265_HIGH_2;
    case VideoLevel::H265_MAIN_21:
      return CNVIDEOENC_LEVEL_H265_MAIN_21;
    case VideoLevel::H265_HIGH_21:
      return CNVIDEOENC_LEVEL_H265_HIGH_21;
    case VideoLevel::H265_MAIN_3:
      return CNVIDEOENC_LEVEL_H265_MAIN_3;
    case VideoLevel::H265_HIGH_3:
      return CNVIDEOENC_LEVEL_H265_HIGH_3;
    case VideoLevel::H265_MAIN_31:
      return CNVIDEOENC_LEVEL_H265_MAIN_31;
    case VideoLevel::H265_HIGH_31:
      return CNVIDEOENC_LEVEL_H265_HIGH_31;
    case VideoLevel::H265_MAIN_4:
      return CNVIDEOENC_LEVEL_H265_MAIN_4;
    case VideoLevel::H265_HIGH_4:
      return CNVIDEOENC_LEVEL_H265_HIGH_4;
    case VideoLevel::H265_MAIN_41:
      return CNVIDEOENC_LEVEL_H265_MAIN_41;
    case VideoLevel::H265_HIGH_41:
      return CNVIDEOENC_LEVEL_H265_HIGH_41;
    case VideoLevel::H265_MAIN_5:
      return CNVIDEOENC_LEVEL_H265_MAIN_5;
    case VideoLevel::H265_HIGH_5:
      return CNVIDEOENC_LEVEL_H265_HIGH_5;
    case VideoLevel::H265_MAIN_51:
      return CNVIDEOENC_LEVEL_H265_MAIN_51;
    case VideoLevel::H265_HIGH_51:
      return CNVIDEOENC_LEVEL_H265_HIGH_51;
    case VideoLevel::H265_MAIN_52:
      return CNVIDEOENC_LEVEL_H265_MAIN_52;
    case VideoLevel::H265_HIGH_52:
      return CNVIDEOENC_LEVEL_H265_HIGH_52;
    case VideoLevel::H265_MAIN_6:
      return CNVIDEOENC_LEVEL_H265_MAIN_6;
    case VideoLevel::H265_HIGH_6:
      return CNVIDEOENC_LEVEL_H265_HIGH_6;
    case VideoLevel::H265_MAIN_61:
      return CNVIDEOENC_LEVEL_H265_MAIN_61;
    case VideoLevel::H265_HIGH_61:
      return CNVIDEOENC_LEVEL_H265_HIGH_61;
    case VideoLevel::H265_MAIN_62:
      return CNVIDEOENC_LEVEL_H265_MAIN_62;
    case VideoLevel::H265_HIGH_62:
      return CNVIDEOENC_LEVEL_H265_HIGH_62;
    default:
      return CNVIDEOENC_LEVEL_MAX;
  }
}

static cnvideoEncGopType GopTypeCast(GopType type) {
  switch (type) {
    case GopType::BIDIRECTIONAL:
      return CNVIDEOENC_GOP_TYPE_BIDIRECTIONAL;
    case GopType::LOW_DELAY:
      return CNVIDEOENC_GOP_TYPE_LOW_DELAY;
    case GopType::PYRAMID:
      return CNVIDEOENC_GOP_TYPE_PYRAMID;
    default:
      return CNVIDEOENC_GOP_TYPE_MAX;
  }
}

static void PrintCreateAttr(cnvideoEncCreateInfo *p_attr) {
  printf("%-32s%s\n", "param", "value");
  printf("-------------------------------------\n");
  printf("%-32s%u\n", "Codectype", p_attr->codec);
  printf("%-32s%u\n", "PixelFormat", p_attr->pixelFmt);
  printf("%-32s%u\n", "Instance", p_attr->instance);
  printf("%-32s%u\n", "DeviceID", p_attr->deviceId);
  printf("%-32s%u\n", "MemoryAllocType", p_attr->allocType);
  printf("%-32s%u\n", "Width", p_attr->width);
  printf("%-32s%u\n", "Height", p_attr->height);
  printf("%-32s%u\n", "FrameRateNum", p_attr->fpsNumerator);
  printf("%-32s%u\n", "FrameRateDen", p_attr->fpsDenominator);
  printf("%-32s%u\n", "ColorSpaceStandard", p_attr->colorSpace);
  printf("%-32s%u\n", "RateCtrlMode", p_attr->rateCtrl.rcMode);
  printf("%-32s%u\n", "InputBufferNumber", p_attr->inputBufNum);
  printf("%-32s%u\n", "OutputBufferNumber", p_attr->outputBufNum);
}

static void PrintCreateAttr(cnjpegEncCreateInfo *p_attr) {
  printf("%-32s%s\n", "param", "value");
  printf("-------------------------------------\n");
  printf("%-32s%u\n", "PixelFormat", p_attr->pixelFmt);
  printf("%-32s%u\n", "Instance", p_attr->instance);
  printf("%-32s%u\n", "DeviceID", p_attr->deviceId);
  printf("%-32s%u\n", "MemoryAllocType", p_attr->allocType);
  printf("%-32s%u\n", "Width", p_attr->width);
  printf("%-32s%u\n", "Height", p_attr->height);
  printf("%-32s%u\n", "ColorSpaceStandard", p_attr->colorSpace);
  printf("%-32s%u\n", "InputBufferNumber", p_attr->inputBufNum);
  printf("%-32s%u\n", "OutputBufferNumber", p_attr->outputBufNum);
  printf("%-32s%u\n", "SuggestedOutputBufferSize", p_attr->suggestedLibAllocBitStrmBufSize);
}

class EncodeHandler {
 public:
  EncodeHandler(EasyEncode *encoder, const EasyEncode::Attr &attr);
  ~EncodeHandler();
  void ReceivePacket(void *packet);
  void ReceiveEOS();
  void AbortEncoder();
  void InitJpegEncode();
  void InitVideoEncode();
  bool SendJpegData(const CnFrame &frame, bool eos);
  bool SendVideoData(const CnFrame &frame, bool eos);
  void CopyFrame(cncodecFrame *dst, const CnFrame &input);
  void ReceiveEvent(cncodecCbEventType type);

  EasyEncode::Attr attr_;
  bool send_eos_ = false;
  bool jpeg_encode_ = false;

 private:
  void EventTaskRunner();

  std::queue<cncodecCbEventType> event_queue_;
  std::mutex event_mtx_;
  std::condition_variable event_cond_;
  std::thread event_loop_;

  cnvideoEncCreateInfo vcreate_params_;
  cnjpegEncCreateInfo jcreate_params_;
  EasyEncode *encoder_ = nullptr;
  const FormatInfo *pixel_fmt_info_ = nullptr;

  void *handle_ = nullptr;
  uint64_t packet_cnt_ = 0;
  bool got_eos_ = false;
  std::mutex eos_mutex_;
  std::condition_variable eos_cond_;
};  // class EncodeHandler

static int32_t EventHandler(cncodecCbEventType type, void *user_data, void *package);

EncodeHandler::EncodeHandler(EasyEncode *encoder, const EasyEncode::Attr &attr) : attr_(attr), encoder_(encoder) {
  jpeg_encode_ = CodecType::JPEG == attr.codec_type;
  pixel_fmt_info_ = FormatInfo::GetFormatInfo(attr.pixel_format);
  if (jpeg_encode_) {
    InitJpegEncode();
  } else {
    InitVideoEncode();
  }
  event_loop_ = std::thread(&EncodeHandler::EventTaskRunner, this);
}

void EncodeHandler::InitVideoEncode() {
  // 1. create params
  vcreate_params_.width = attr_.frame_geometry.w;
  vcreate_params_.height = attr_.frame_geometry.h;
  vcreate_params_.deviceId = attr_.dev_id;
  vcreate_params_.pixelFmt = pixel_fmt_info_->cncodec_fmt;
  vcreate_params_.colorSpace = ColorStdCast(attr_.color_std);
  vcreate_params_.codec = CodecTypeCast(attr_.codec_type);
  vcreate_params_.instance = CNVIDEOENC_INSTANCE_AUTO;
  vcreate_params_.userContext = reinterpret_cast<void *>(this);
  vcreate_params_.inputBuf = nullptr;
  vcreate_params_.outputBuf = nullptr;
  vcreate_params_.inputBufNum = attr_.input_buffer_num;    // 6
  vcreate_params_.outputBufNum = attr_.output_buffer_num;  // 6
  vcreate_params_.allocType = CNCODEC_BUF_ALLOC_LIB;
  vcreate_params_.suggestedLibAllocBitStrmBufSize = g_buffer_size;

  memset(&vcreate_params_.rateCtrl, 0x0, sizeof(vcreate_params_.rateCtrl));
  if (attr_.rate_control.vbr) {
    vcreate_params_.rateCtrl.rcMode = CNVIDEOENC_RATE_CTRL_VBR;
  } else {
    vcreate_params_.rateCtrl.rcMode = CNVIDEOENC_RATE_CTRL_CBR;
  }
  vcreate_params_.fpsNumerator = attr_.rate_control.frame_rate_num;
  vcreate_params_.fpsDenominator = attr_.rate_control.frame_rate_den;
  vcreate_params_.rateCtrl.targetBitrate = attr_.rate_control.bit_rate;
  vcreate_params_.rateCtrl.peakBitrate = attr_.rate_control.max_bit_rate;
  vcreate_params_.rateCtrl.gopLength = attr_.rate_control.gop;
  vcreate_params_.rateCtrl.maxIQP = attr_.rate_control.max_qp;
  vcreate_params_.rateCtrl.maxPQP = attr_.rate_control.max_qp;
  vcreate_params_.rateCtrl.maxBQP = attr_.rate_control.max_qp;
  vcreate_params_.rateCtrl.minIQP = attr_.rate_control.min_qp;
  vcreate_params_.rateCtrl.minPQP = attr_.rate_control.min_qp;
  vcreate_params_.rateCtrl.minBQP = attr_.rate_control.min_qp;

  if (vcreate_params_.codec == CNCODEC_H264) {
    memset(&vcreate_params_.uCfg.h264, 0x0, sizeof(vcreate_params_.uCfg.h264));
    if (static_cast<int>(attr_.profile) > static_cast<int>(VideoProfile::H264_HIGH_10)) {
      LOGW(ENCODE) << "Invalid H264 profile, using H264_MAIN as default";
      vcreate_params_.uCfg.h264.profile = CNVIDEOENC_PROFILE_H264_HIGH;
    } else {
      vcreate_params_.uCfg.h264.profile = ProfileCast(attr_.profile);
    }
    if (static_cast<int>(attr_.level) > static_cast<int>(VideoLevel::H264_51)) {
      LOGW(ENCODE) << "Invalid H264 level, using H264_41 as default";
      vcreate_params_.uCfg.h264.level = CNVIDEOENC_LEVEL_H264_41;
    } else {
      vcreate_params_.uCfg.h264.level = LevelCast(attr_.level);
    }
    vcreate_params_.uCfg.h264.IframeInterval = attr_.p_frame_num;
    vcreate_params_.uCfg.h264.BFramesNum = attr_.b_frame_num;
    vcreate_params_.uCfg.h264.insertSpsPpsWhenIDR = attr_.insert_spspps_when_idr ? 1 : 0;
    vcreate_params_.uCfg.h264.gopType = GopTypeCast(attr_.gop_type);
    vcreate_params_.uCfg.h264.entropyMode = CNVIDEOENC_ENTROPY_MODE_CABAC;
  } else if (vcreate_params_.codec == CNCODEC_HEVC) {
    memset(&vcreate_params_.uCfg.h265, 0x0, sizeof(vcreate_params_.uCfg.h265));
    if (static_cast<int>(attr_.profile) < static_cast<int>(VideoProfile::H265_MAIN)) {
      LOGW(ENCODE) << "Invalid H265 profile, using H265_MAIN as default";
      vcreate_params_.uCfg.h265.profile = CNVIDEOENC_PROFILE_H265_MAIN;
    } else {
      vcreate_params_.uCfg.h265.profile = ProfileCast(attr_.profile);
    }
    if (static_cast<int>(attr_.level) < static_cast<int>(VideoLevel::H265_MAIN_1)) {
      LOGW(ENCODE) << "Invalid H265 level, using H265_MAIN_41 as default";
      vcreate_params_.uCfg.h265.level = CNVIDEOENC_LEVEL_H265_HIGH_41;
    } else {
      vcreate_params_.uCfg.h265.level = LevelCast(attr_.level);
    }
    vcreate_params_.uCfg.h265.IframeInterval = attr_.p_frame_num;
    vcreate_params_.uCfg.h265.BFramesNum = attr_.b_frame_num;
    vcreate_params_.uCfg.h265.insertSpsPpsWhenIDR = attr_.insert_spspps_when_idr ? 1 : 0;
    vcreate_params_.uCfg.h265.gopType = GopTypeCast(attr_.gop_type);
  } else {
    THROW_EXCEPTION(Exception::UNSUPPORTED, "Encoder only support format H264/H265/JPEG");
  }

  if (!attr_.silent) {
    PrintCreateAttr(&vcreate_params_);
  }

  int ecode = cnvideoEncCreate(reinterpret_cast<cnvideoEncoder *>(&handle_), EventHandler, &vcreate_params_);
  if (CNCODEC_SUCCESS != ecode) {
    handle_ = nullptr;
    THROW_EXCEPTION(Exception::INIT_FAILED, "Initialize video encoder failed. cncodec error code: " + to_string(ecode));
  }
  LOGI(ENCODE) << "Init video encoder succeeded";
}

void EncodeHandler::InitJpegEncode() {
  // 1. create params
  jcreate_params_.deviceId = attr_.dev_id;
  jcreate_params_.instance = CNJPEGENC_INSTANCE_AUTO;
  jcreate_params_.pixelFmt = pixel_fmt_info_->cncodec_fmt;
  jcreate_params_.colorSpace = ColorStdCast(attr_.color_std);
  jcreate_params_.width = attr_.frame_geometry.w;
  jcreate_params_.height = attr_.frame_geometry.h;
  jcreate_params_.inputBuf = nullptr;
  jcreate_params_.outputBuf = nullptr;
  jcreate_params_.inputBufNum = attr_.input_buffer_num;    // 6
  jcreate_params_.outputBufNum = attr_.output_buffer_num;  // 6
  jcreate_params_.allocType = CNCODEC_BUF_ALLOC_LIB;
  jcreate_params_.userContext = reinterpret_cast<void *>(this);
  jcreate_params_.suggestedLibAllocBitStrmBufSize = g_buffer_size;

  if (!attr_.silent) {
    PrintCreateAttr(&jcreate_params_);
  }

  int ecode = cnjpegEncCreate(reinterpret_cast<cnjpegEncoder *>(&handle_), CNJPEGENC_RUN_MODE_ASYNC, EventHandler,
                              &jcreate_params_);
  if (CNCODEC_SUCCESS != ecode) {
    handle_ = nullptr;
    THROW_EXCEPTION(Exception::INIT_FAILED, "Initialize jpeg encoder failed. cncodec error code: " + to_string(ecode));
  }
  LOGI(ENCODE) << "Init JPEG encoder succeeded";
}

EncodeHandler::~EncodeHandler() {
  try {
    std::unique_lock<std::mutex> eos_lk(eos_mutex_);
    if (!got_eos_) {
      if (!send_eos_ && handle_) {
        eos_lk.unlock();
        LOGI(ENCODE) << "Send EOS in destruct";
        encoder_->SendDataCPU({}, true);
      } else {
        if (!handle_) got_eos_ = true;
      }
    }

    if (!eos_lk.owns_lock()) {
      eos_lk.lock();
    }

    if (!got_eos_) {
      LOGI(ENCODE) << "Wait EOS in destruct";
      eos_cond_.wait(eos_lk, [this]() -> bool { return got_eos_; });
    }

    event_cond_.notify_all();
    if (event_loop_.joinable()) {
      event_loop_.join();
    }
    // destroy encoder
    if (handle_) {
      int ecode;
      if (jpeg_encode_) {
        ecode = cnjpegEncDestroy(reinterpret_cast<cnjpegEncoder>(handle_));
      } else {
        ecode = cnvideoEncDestroy(reinterpret_cast<cnvideoEncoder>(handle_));
      }
      if (CNCODEC_SUCCESS != ecode) {
        LOGE(ENCODE) << "Destroy encoder failed. Error code: " << ecode;
      }
    }
  } catch (std::system_error &e) {
    LOGE(ENCODE) << e.what();
  } catch (Exception &e) {
    LOGE(ENCODE) << e.what();
  }
}

void EncodeHandler::ReceivePacket(void *_packet) {
  LOGT(ENCODE) << "Encode receive packet " << _packet;
  // packet callback
  if (attr_.packet_callback) {
    CnPacket cn_packet;
    if (jpeg_encode_) {
      auto packet = reinterpret_cast<cnjpegEncOutput *>(_packet);
      if (packet->result != 0) {
        LOGE(ENCODE) << "Encode receive a wrong packet. pts [" << packet->pts << "]";
      }
      cn_packet.data = new uint8_t[packet->streamLength];
      auto ret = cnrtMemcpy(cn_packet.data, reinterpret_cast<void *>(packet->streamBuffer.addr + packet->dataOffset),
                            packet->streamLength, CNRT_MEM_TRANS_DIR_DEV2HOST);
      if (ret != CNRT_RET_SUCCESS) {
        LOGE(ENCODE) << "Copy bitstream failed, DEV2HOST";
        AbortEncoder();
        return;
      }
      cn_packet.length = packet->streamLength;
      cn_packet.pts = packet->pts;
      cn_packet.codec_type = attr_.codec_type;
      cn_packet.buf_id = reinterpret_cast<uint64_t>(cn_packet.data);
      cn_packet.slice_type = BitStreamSliceType::FRAME;
      ++packet_cnt_;
    } else {
      auto packet = reinterpret_cast<cnvideoEncOutput *>(_packet);
      cn_packet.data = new uint8_t[packet->streamLength];
      auto ret = cnrtMemcpy(cn_packet.data, reinterpret_cast<void *>(packet->streamBuffer.addr + packet->dataOffset),
                            packet->streamLength, CNRT_MEM_TRANS_DIR_DEV2HOST);
      if (ret != CNRT_RET_SUCCESS) {
        LOGE(ENCODE) << "Copy bitstream failed, DEV2HOST";
        AbortEncoder();
        return;
      }
      cn_packet.length = packet->streamLength;
      cn_packet.pts = packet->pts;
      cn_packet.codec_type = attr_.codec_type;
      cn_packet.buf_id = reinterpret_cast<uint64_t>(cn_packet.data);
      if (packet_cnt_++) {
        cn_packet.slice_type = BitStreamSliceType::FRAME;
      } else {
        cn_packet.slice_type = BitStreamSliceType::SPS_PPS;
      }
    }

    attr_.packet_callback(cn_packet);
  }
}

void EncodeHandler::ReceiveEOS() {
  // eos callback
  LOGI(ENCODE) << "Encode receive EOS";

  if (attr_.eos_callback) {
    attr_.eos_callback();
  }

  std::lock_guard<std::mutex> lk(eos_mutex_);
  got_eos_ = true;
  eos_cond_.notify_one();
}

void EncodeHandler::CopyFrame(cncodecFrame *dst, const CnFrame &input) {
  uint32_t frame_size = 0, uv_size = 0;
  if (input.strides[0] == 0) {
    frame_size = input.width * input.height;
  } else {
    frame_size = input.strides[0] * input.height;
  }
  if (input.strides[1] == 0) {
    uv_size = input.width * input.height >> 1;
  } else {
    uv_size = input.strides[1] * input.height >> 1;
  }

  if (input.frame_size > 0) {
    MluMemoryOp mem_op;
    // cnrtRet_t cnrt_ecode = CNRT_RET_SUCCESS;
    switch (attr_.pixel_format) {
      case PixelFmt::NV12:
      case PixelFmt::NV21: {
        LOGT(ENCODE) << "Copy frame luminance";
        mem_op.MemcpyH2D(reinterpret_cast<void *>(dst->plane[0].addr), input.ptrs[0], frame_size);
        LOGT(ENCODE) << "Copy frame chroma";
        mem_op.MemcpyH2D(reinterpret_cast<void *>(dst->plane[1].addr), input.ptrs[1], uv_size);
        break;
      }
      case PixelFmt::I420: {
        LOGT(ENCODE) << "Copy frame luminance";
        mem_op.MemcpyH2D(reinterpret_cast<void *>(dst->plane[0].addr), input.ptrs[0], frame_size);
        LOGT(ENCODE) << "Copy frame chroma 0";
        mem_op.MemcpyH2D(reinterpret_cast<void *>(dst->plane[1].addr), input.ptrs[1], uv_size >> 1);
        LOGT(ENCODE) << "Copy frame chroma 1";
        mem_op.MemcpyH2D(reinterpret_cast<void *>(dst->plane[2].addr), input.ptrs[2], uv_size >> 1);
        break;
      }
      case PixelFmt::ARGB:
      case PixelFmt::ABGR:
      case PixelFmt::RGBA:
      case PixelFmt::BGRA:
        LOGT(ENCODE) << "Copy frame RGB family";
        mem_op.MemcpyH2D(reinterpret_cast<void *>(dst->plane[0].addr), input.ptrs[0], frame_size << 2);
        break;
      default:
        THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported pixel format");
        break;
    }
  }
}

bool EncodeHandler::SendJpegData(const CnFrame &frame, bool eos) {
  cnjpegEncInput input;
  cnjpegEncParameters params;
  memset(&input, 0, sizeof(cnjpegEncInput));
  int ecode = cnjpegEncWaitAvailInputBuf(reinterpret_cast<cnjpegEncoder>(handle_), &input.frame, 10000);
  if (-CNCODEC_TIMEOUT == ecode) {
    LOGE(ENCODE) << "cnjpegEncWaitAvailInputBuf timeout";
    return false;
  } else if (CNCODEC_SUCCESS != ecode) {
    LOGE(ENCODE) << "Get jpeg enc avaliable input buffer failed. Error code: " + to_string(ecode);
    return false;
  }

  // 2. copy data to codec
  CopyFrame(&input.frame, frame);

  // 3. set params for codec
  if (eos) {
    input.flags |= CNJPEGENC_FLAG_EOS;
    send_eos_ = true;
    if (frame.width * frame.height == 0) {
      input.flags |= CNJPEGENC_FLAG_INVALID_FRAME;
    }
  } else {
    input.flags &= (~CNJPEGENC_FLAG_EOS);
  }
  LOGT(ENCODE) << "Feed jpeg frame info) data: " << frame.ptrs[0] << " length: " << frame.frame_size;

  input.frame.pixelFmt = jcreate_params_.pixelFmt;
  input.frame.colorSpace = jcreate_params_.colorSpace;
  input.frame.width = frame.width;
  input.frame.height = frame.height;
  input.pts = frame.pts;
  for (uint32_t i = 0; i < frame.n_planes; ++i) {
    input.frame.stride[i] = frame.strides[i];
  }
  params.quality = attr_.jpeg_qfactor;
  params.restartInterval = 0;
  // 4. send data to codec
  ecode = cnjpegEncFeedFrame(reinterpret_cast<cnjpegEncoder>(handle_), &input, &params, 10000);
  if (-CNCODEC_TIMEOUT == ecode) {
    LOGE(ENCODE) << "cnjpegEncFeedData timeout";
    return false;
  } else if (CNCODEC_SUCCESS != ecode) {
    THROW_EXCEPTION(Exception::INTERNAL, "cnjpegEncFeedFrame failed. cncodec error code: " + to_string(ecode));
  }

  return true;
}

bool EncodeHandler::SendVideoData(const CnFrame &frame, bool eos) {
  cnvideoEncInput input;
  memset(&input, 0, sizeof(cnvideoEncInput));
  int ecode = cnvideoEncWaitAvailInputBuf(reinterpret_cast<cnvideoEncoder>(handle_), &input.frame, 10000);
  if (-CNCODEC_TIMEOUT == ecode) {
    LOGE(ENCODE) << "cnvideoEncWaitAvailInputBuf timeout";
    return false;
  } else if (CNCODEC_SUCCESS != ecode) {
    LOGE(ENCODE) << "Get video enc avaliable input buffer failed. Error code: " + to_string(ecode);
    return false;
  }

  // 2. copy data to codec
  CopyFrame(&input.frame, frame);

  // 3. set params for codec
  if (eos) {
    input.flags |= CNVIDEOENC_FLAG_EOS;
    send_eos_ = true;
    if (frame.width * frame.height == 0) {
      input.flags |= CNVIDEOENC_FLAG_INVALID_FRAME;
    }
  } else {
    input.flags &= (~CNVIDEOENC_FLAG_EOS);
  }
  LOGT(ENCODE) << "Feed video frame info) data: " << frame.ptrs[0] << " length: " << frame.frame_size
               << " pts: " << frame.pts;

  input.frame.pixelFmt = vcreate_params_.pixelFmt;
  input.frame.colorSpace = vcreate_params_.colorSpace;
  input.frame.width = frame.width;
  input.frame.height = frame.height;
  input.pts = frame.pts;
  for (uint32_t i = 0; i < frame.n_planes; ++i) {
    input.frame.stride[i] = frame.strides[i];
  }
  // 4. send data to codec
  ecode = cnvideoEncFeedFrame(reinterpret_cast<cnvideoEncoder>(handle_), &input, 10000);
  if (-CNCODEC_TIMEOUT == ecode) {
    LOGE(ENCODE) << "cnvideoEncFeedData timeout";
    return false;
  } else if (CNCODEC_SUCCESS != ecode) {
    THROW_EXCEPTION(Exception::INTERNAL, "cnvideoEncFeedFrame failed. cncodec error code: " + to_string(ecode));
  }
  return true;
}

void EncodeHandler::AbortEncoder() {
  LOGW(ENCODE) << "Abort encoder";
  if (handle_) {
    if (jpeg_encode_) {
      cnjpegEncAbort(handle_);
    } else {
      cnvideoEncAbort(handle_);
    }
    handle_ = nullptr;

    if (attr_.eos_callback) {
      attr_.eos_callback();
    }
    std::unique_lock<std::mutex> eos_lk(eos_mutex_);
    got_eos_ = true;
    eos_cond_.notify_one();
  } else {
    LOGE(ENCODE) << "Won't do abort, since cnencode handler has not been initialized";
  }
}

void EncodeHandler::ReceiveEvent(cncodecCbEventType type) {
  std::lock_guard<std::mutex> lock(event_mtx_);
  event_queue_.push(type);
  event_cond_.notify_one();
}

void EncodeHandler::EventTaskRunner() {
  std::unique_lock<std::mutex> lock(event_mtx_);
  while (!event_queue_.empty() || !got_eos_) {
    event_cond_.wait(lock, [this] { return !event_queue_.empty() || got_eos_; });

    if (event_queue_.empty()) {
      // notified by eos
      continue;
    }

    cncodecCbEventType type = event_queue_.front();
    event_queue_.pop();
    lock.unlock();

    switch (type) {
      case CNCODEC_CB_EVENT_EOS:
        ReceiveEOS();
        break;
      case CNCODEC_CB_EVENT_SW_RESET:
      case CNCODEC_CB_EVENT_HW_RESET:
        LOGE(ENCODE) << "Encode firmware crash event: " << type;
        AbortEncoder();
        break;
      case CNCODEC_CB_EVENT_OUT_OF_MEMORY:
        LOGE(ENCODE) << "Out of memory error thrown from cncodec";
        AbortEncoder();
        break;
      case CNCODEC_CB_EVENT_ABORT_ERROR:
        LOGE(ENCODE) << "Abort error thrown from cncodec";
        AbortEncoder();
        break;
#if CNCODEC_VERSION >= 10600
      case CNCODEC_CB_EVENT_STREAM_CORRUPT:
        LOGW(ENCODE) << "Stream corrupt, discard frame";
        break;
#endif
      default:
        LOGE(ENCODE) << "Unknown event type";
        AbortEncoder();
        break;
    }
    lock.lock();
  }
}

static int32_t EventHandler(cncodecCbEventType type, void *user_data, void *package) {
  auto handler = reinterpret_cast<EncodeHandler *>(user_data);
  if (handler != nullptr) {
    switch (type) {
      case CNCODEC_CB_EVENT_NEW_FRAME:
        handler->ReceivePacket(package);
        break;
      default:
        handler->ReceiveEvent(type);
        break;
    }
  }
  return 0;
}

std::unique_ptr<EasyEncode> EasyEncode::New(const Attr &attr) {
  struct __ShowCodecVersion {
    __ShowCodecVersion() {
      u8_t *version = cncodecGetVersion();
      LOGI(ENCODE) << "CNCodec Version: " << static_cast<const unsigned char *>(version);
    }
  };
  static __ShowCodecVersion show_version;

  LOGI(ENCODE) << "Create EasyEncode";
  auto encoder = new EasyEncode();
  try {
    encoder->handler_ = new EncodeHandler(encoder, attr);
  } catch (Exception &e) {
    LOGE(ENCODE) << "Create encode failed, error message: " << e.what();
    delete encoder;
    return nullptr;
  }
  return std::unique_ptr<EasyEncode>(encoder);
}

EasyEncode::EasyEncode() {}

EasyEncode::~EasyEncode() {
  // free members
  if (handler_) {
    delete handler_;
    handler_ = nullptr;
  }
}

void EasyEncode::AbortEncoder() { handler_->AbortEncoder(); }

EasyEncode::Attr EasyEncode::GetAttr() const { return handler_->attr_; }

void EasyEncode::ReleaseBuffer(uint64_t buf_id) {
  LOGD(ENCODE) << "Release buffer, " << reinterpret_cast<void *>(buf_id);
  delete[] reinterpret_cast<uint8_t *>(buf_id);
}

bool EasyEncode::SendDataCPU(const CnFrame &frame, bool eos) {
  bool ret = false;
  if (!handler_) {
    LOGE(ENCODE) << "Encoder has not been init";
    return false;
  }
  if (handler_->send_eos_) {
    LOGW(ENCODE) << "EOS had been sent, won't feed data or EOS";
    return false;
  }

  if (handler_->jpeg_encode_) {
    ret = handler_->SendJpegData(frame, eos);
  } else {
    ret = handler_->SendVideoData(frame, eos);
  }

  return ret;
}

}  // namespace edk
