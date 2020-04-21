/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#include "avutil/byte_rw.h"
#include "avformat/asf_rw.h"

static const uint8_t _guid_header[ASF_GUID_SIZE] = {
    0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c
};

static const uint8_t _guid_file_header[ASF_GUID_SIZE] = {
    0xa1, 0xdc, 0xab, 0x8c, 0x47, 0xa9, 0xcf, 0x11, 0x8e, 0xe4, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65
};

static const uint8_t _guid_stream_header[ASF_GUID_SIZE] = {
    0x91, 0x07, 0xdc, 0xb7, 0xb7, 0xa9, 0xcf, 0x11, 0x8e, 0xe6, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65
};

static const uint8_t _guid_audio_stream[ASF_GUID_SIZE] = {
    0x40, 0x9e, 0x69, 0xf8, 0x4d, 0x5b, 0xcf, 0x11, 0xa8, 0xfd, 0x00, 0x80, 0x5f, 0x5c, 0x44, 0x2b
};

static const uint8_t _guid_data_header[ASF_GUID_SIZE] = {
    0x36, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c
};

#if 0
static const uint8_t _guid_ext_stream_header[ASF_GUID_SIZE] = {
    0xcb, 0xa5, 0xe6, 0x14, 0x72, 0xc6, 0x32, 0x43, 0x83, 0x99, 0xa9, 0x69, 0x52, 0x06, 0x5b, 0x5a
};

static const uint8_t _guid_audio_conceal_spread[ASF_GUID_SIZE] = {
    0x50, 0xcd, 0xc3, 0xbf, 0x8f, 0x61, 0xcf, 0x11, 0x8b, 0xb2, 0x00, 0xaa, 0x00, 0xb4, 0xe2, 0x20
};

static const uint8_t _guid_audio_conceal_none[ASF_GUID_SIZE] = {
    0x00, 0x57, 0xfb, 0x20, 0x55, 0x5b, 0xcf, 0x11, 0xa8, 0xfd, 0x00, 0x80, 0x5f, 0x5c, 0x44, 0x2b
};

static const uint8_t _guid_head1_guid[ASF_GUID_SIZE] = {
    0xb5, 0x03, 0xbf, 0x5f, 0x2e, 0xa9, 0xcf, 0x11, 0x8e, 0xe3, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65
};

static const uint8_t _guid_head2_guid[ASF_GUID_SIZE] = {
    0x11, 0xd2, 0xd3, 0xab, 0xba, 0xa9, 0xcf, 0x11, 0x8e, 0xe6, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65
};

static const uint8_t _guid_ext_stream_audio_stream[ASF_GUID_SIZE] = {
    0x9d, 0x8c, 0x17, 0x31, 0xe1, 0x03, 0x28, 0x45, 0xb5, 0x82, 0x3d, 0xf9, 0xdb, 0x22, 0xf5, 0x03
};

static const uint8_t _guid_metadata_header[ASF_GUID_SIZE] = {
    0xea, 0xcb, 0xf8, 0xc5, 0xaf, 0x5b, 0x77, 0x48, 0x84, 0x67, 0xaa, 0x8c, 0x44, 0xfa, 0x4c, 0xca
};

static const uint8_t _guid_extended_stream_properties_object[ASF_GUID_SIZE] = {
    0xcb, 0xa5, 0xe6, 0x14, 0x72, 0xc6, 0x32, 0x43, 0x83, 0x99, 0xa9, 0x69, 0x52, 0x06, 0x5b, 0x5a
};

#endif

/**
* @brief  get asf guid type
* @param  [in] guid
* @return ASF_GUID_UNKNOWN on err
*/
asf_guid_t asf_guid_get_type(const uint8_t guid[ASF_GUID_SIZE])
{
    asf_guid_t ret = ASF_GUID_UNKNOWN;

    CHECK_PARAM(guid, ret);
    if (IS_ASF_GUID_EQUAL(guid, &_guid_header))
        ret = ASF_GUID_HEADER;
    else if (IS_ASF_GUID_EQUAL(guid, &_guid_data_header))
        ret = ASF_GUID_DATA;
    else if (IS_ASF_GUID_EQUAL(guid, &_guid_file_header))
        ret = ASF_GUID_FILE_PROPERTIES;
    else if (IS_ASF_GUID_EQUAL(guid, &_guid_stream_header))
        ret = ASF_GUID_STREAM_PROPERTIES;
#if 0
    else if (IS_ASF_GUID_EQUAL(guid, &_guid_padding))
        ret = ASF_GUID_PADDING;
    else if (IS_ASF_GUID_EQUAL(guid, &_guid_metadata))
        ret = ASF_GUID_METADATA;
#endif
    else if (IS_ASF_GUID_EQUAL(guid, &_guid_audio_stream))
        ret = ASF_GUID_STREAM_TYPE_AUDIO;

    return ret;
}



