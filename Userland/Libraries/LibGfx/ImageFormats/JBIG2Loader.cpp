/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/ImageFormats/JBIG2Loader.h>

// Spec: ITU-T_T_88__08_2018.pdf in the zip file here:
// https://www.itu.int/rec/T-REC-T.88-201808-I
// Annex H has a datastream example.

namespace Gfx {

// JBIG2 spec, Annex D, D.4.1 ID string
static constexpr u8 id_string[] = { 0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A };

// 7.3 Segment types
enum SegmentType {
    SymbolDictionary = 0,
    IntermediateTextRegion = 4,
    ImmediateTextRegion = 6,
    ImmediateLosslessTextRegion = 7,
    PatternDictionary = 16,
    IntermediateHalftoneRegion = 20,
    ImmediateHalftoneRegion = 22,
    ImmediateLosslessHalftoneRegion = 23,
    IntermediateGenericRegion = 36,
    ImmediateGenericRegion = 38,
    ImmediateLosslessGenericRegion = 39,
    IntermediateGenericRefinementRegion = 40,
    ImmediateGenericRefinementRegion = 42,
    ImmediateLosslessGenericRefinementRegion = 43,
    PageInformation = 48,
    EndOfPage = 49,
    EndOfStripe = 50,
    EndOfFile = 51,
    Profiles = 52,
    Tables = 53,
    ColorPalette = 54,
    Extension = 62,
};

// Annex D
enum class Organization {
    // D.1 Sequential organization
    Sequential,

    // D.2 Random-access organization
    RandomAccess,

    // D.3 Embedded organization
    Embedded,
};

struct SegmentHeader {
    u32 segment_number;
    SegmentType type;
    Vector<u32> referred_to_segment_numbers;
    u32 page_association;
    Optional<u32> data_length;
};

struct SegmentData {
    SegmentHeader header;
    ReadonlyBytes data;
};

struct JBIG2LoadingContext {
    enum class State {
        NotDecoded = 0,
        Error,
    };
    State state { State::NotDecoded };

    Organization organization { Organization::Sequential };
    IntSize size;

    Optional<u32> number_of_pages;

    Vector<SegmentData> segments;
};

static ErrorOr<void> decode_jbig2_header(JBIG2LoadingContext& context, ReadonlyBytes data)
{
    if (!JBIG2ImageDecoderPlugin::sniff(data))
        return Error::from_string_literal("JBIG2LoadingContext: Invalid JBIG2 header");

    FixedMemoryStream stream(data.slice(sizeof(id_string)));

    // D.4.2 File header flags
    u8 header_flags = TRY(stream.read_value<u8>());
    if (header_flags & 0b11110000)
        return Error::from_string_literal("JBIG2LoadingContext: Invalid header flags");
    context.organization = (header_flags & 1) ? Organization::Sequential : Organization::RandomAccess;
    dbgln_if(JBIG2_DEBUG, "JBIG2LoadingContext: Organization: {} ({})", (int)context.organization, context.organization == Organization::Sequential ? "Sequential" : "Random-access");
    bool has_known_number_of_pages = (header_flags & 2) ? false : true;
    bool uses_templates_with_12_AT_pixels = (header_flags & 4) ? true : false;
    bool contains_colored_region_segments = (header_flags & 8) ? true : false;

    // FIXME: Do something with these?
    (void)uses_templates_with_12_AT_pixels;
    (void)contains_colored_region_segments;

    // D.4.3 Number of pages
    if (has_known_number_of_pages) {
        context.number_of_pages = TRY(stream.read_value<BigEndian<u32>>());
        dbgln_if(JBIG2_DEBUG, "JBIG2LoadingContext: Number of pages: {}", context.number_of_pages.value());
    }

    return {};
}

static ErrorOr<SegmentHeader> decode_segment_header(SeekableStream& stream)
{
    // 7.2.2 Segment number
    u32 segment_number = TRY(stream.read_value<BigEndian<u32>>());
    dbgln_if(JBIG2_DEBUG, "Segment number: {}", segment_number);

    // 7.2.3 Segment header flags
    u8 flags = TRY(stream.read_value<u8>());
    SegmentType type = static_cast<SegmentType>(flags & 0b11'1111);
    dbgln_if(JBIG2_DEBUG, "Segment type: {}", (int)type);
    bool segment_page_association_size_is_32_bits = (flags & 0b100'0000) != 0;
    bool segment_retained_only_by_itself_and_extension_segments = (flags & 0b1000'00000) != 0;

    // FIXME: Do something with these.
    (void)segment_page_association_size_is_32_bits;
    (void)segment_retained_only_by_itself_and_extension_segments;

    // 7.2.4 Referred-to segment count and retention flags
    u8 referred_to_segment_count_and_retention_flags = TRY(stream.read_value<u8>());
    u32 count_of_referred_to_segments = referred_to_segment_count_and_retention_flags >> 5;
    if (count_of_referred_to_segments == 5 || count_of_referred_to_segments == 6)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid count_of_referred_to_segments");
    u32 extra_count = 0;
    if (count_of_referred_to_segments == 7) {
        TRY(stream.seek(-1, SeekMode::FromCurrentPosition));
        count_of_referred_to_segments = TRY(stream.read_value<BigEndian<u32>>()) & 0x1FFF'FFFF;
        extra_count = ceil_div(count_of_referred_to_segments + 1, 8);
        TRY(stream.seek(extra_count, SeekMode::FromCurrentPosition));
    }
    dbgln_if(JBIG2_DEBUG, "Referred-to segment count: {}", count_of_referred_to_segments);

    // 7.2.5 Referred-to segment numbers
    Vector<u32> referred_to_segment_numbers;
    for (u32 i = 0; i < count_of_referred_to_segments; ++i) {
        u32 referred_to_segment_number;
        if (segment_number <= 256)
            referred_to_segment_number = TRY(stream.read_value<u8>());
        else if (segment_number <= 65536)
            referred_to_segment_number = TRY(stream.read_value<BigEndian<u16>>());
        else
            referred_to_segment_number = TRY(stream.read_value<BigEndian<u32>>());
        referred_to_segment_numbers.append(referred_to_segment_number);
        dbgln_if(JBIG2_DEBUG, "Referred-to segment number: {}", referred_to_segment_number);
    }

    // 7.2.6 Segment page association
    u32 segment_page_association;
    if (segment_page_association_size_is_32_bits) {
        segment_page_association = TRY(stream.read_value<BigEndian<u32>>());
    } else {
        segment_page_association = TRY(stream.read_value<u8>());
    }
    dbgln_if(JBIG2_DEBUG, "Segment page association: {}", segment_page_association);

    // 7.2.7 Segment data length
    u32 data_length = TRY(stream.read_value<BigEndian<u32>>());
    dbgln_if(JBIG2_DEBUG, "Segment data length: {}", data_length);

    // FIXME: Add some validity checks:
    // - check type is valid
    // - check referred_to_segment_numbers are smaller than segment_number
    // - 7.3.1 Rules for segment references
    // - 7.3.2 Rules for page associations

    Optional<u32> opt_data_length;
    if (data_length != 0xffff'ffff)
        opt_data_length = data_length;
    else if (type != ImmediateGenericRegion)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Unknown data length only allowed for ImmediateGenericRegion");

    return SegmentHeader { segment_number, type, move(referred_to_segment_numbers), segment_page_association, opt_data_length };
}

static ErrorOr<size_t> scan_for_immediate_generic_region_size(ReadonlyBytes data)
{
    // 7.2.7 Segment data length
    // "If the segment's type is "Immediate generic region", then the length field may contain the value 0xFFFFFFFF.
    //  This value is intended to mean that the length of the segment's data part is unknown at the time that the segment header is written (...).
    //  In this case, the true length of the segment's data part shall be determined through examination of the data:
    //  if the segment uses template-based arithmetic coding, then the segment's data part ends with the two-byte sequence 0xFF 0xAC followed by a four-byte row count.
    //  If the segment uses MMR coding, then the segment's data part ends with the two-byte sequence 0x00 0x00 followed by a four-byte row count.
    //  The form of encoding used by the segment may be determined by examining the eighteenth byte of its segment data part,
    //  and the end sequences can occur anywhere after that eighteenth byte."
    // 7.4.6.4 Decoding a generic region segment
    // "NOTE – The sequence 0x00 0x00 cannot occur within MMR-encoded data; the sequence 0xFF 0xAC can occur only at the end of arithmetically-coded data.
    //  Thus, those sequences cannot occur by chance in the data that is decoded to generate the contents of the generic region."
    dbgln_if(JBIG2_DEBUG, "(Unknown data length, computing it)");

    if (data.size() < 18)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Data too short to contain segment data header");

    // Per 7.4.6.1 Generic region segment data header, this starts with the 17 bytes described in
    // 7.4.1 Region segment information field, followed the byte described in 7.4.6.2 Generic region segment flags.
    // That byte's lowest bit stores if the segment uses MMR.
    u8 flags = data[17];
    bool uses_mmr = (flags & 1) != 0;
    auto end_sequence = uses_mmr ? to_array<u8>({ 0x00, 0x00 }) : to_array<u8>({ 0xFF, 0xAC });
    u8 const* end = static_cast<u8 const*>(memmem(data.data() + 19, data.size() - 19 - sizeof(u32), end_sequence.data(), end_sequence.size()));
    if (!end)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Could not find end sequence in segment data");

    size_t size = end - data.data() + end_sequence.size() + sizeof(u32);
    dbgln_if(JBIG2_DEBUG, "(Computed size is {})", size);
    return size;
}

static ErrorOr<void> decode_segment_headers(JBIG2LoadingContext& context, ReadonlyBytes data)
{
    FixedMemoryStream stream(data);

    Vector<ReadonlyBytes> segment_datas;
    auto store_and_skip_segment_data = [&](SegmentHeader const& segment_header) -> ErrorOr<void> {
        size_t start_offset = TRY(stream.tell());
        u32 data_length = TRY(segment_header.data_length.try_value_or_lazy_evaluated([&]() {
            return scan_for_immediate_generic_region_size(data.slice(start_offset));
        }));

        if (start_offset + data_length > data.size()) {
            dbgln_if(JBIG2_DEBUG, "JBIG2ImageDecoderPlugin: start_offset={}, data_length={}, data.size()={}", start_offset, data_length, data.size());
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Segment data length exceeds file size");
        }
        ReadonlyBytes segment_data = data.slice(start_offset, data_length);
        segment_datas.append(segment_data);

        TRY(stream.seek(data_length, SeekMode::FromCurrentPosition));
        return {};
    };

    Vector<SegmentHeader> segment_headers;
    while (!stream.is_eof()) {
        auto segment_header = TRY(decode_segment_header(stream));
        segment_headers.append(segment_header);

        if (context.organization != Organization::RandomAccess)
            TRY(store_and_skip_segment_data(segment_header));

        // Required per spec for files with RandomAccess organization.
        if (segment_header.type == SegmentType::EndOfFile)
            break;
    }

    if (context.organization == Organization::RandomAccess) {
        for (auto const& segment_header : segment_headers)
            TRY(store_and_skip_segment_data(segment_header));
    }

    if (segment_headers.size() != segment_datas.size())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Segment headers and segment datas have different sizes");
    for (size_t i = 0; i < segment_headers.size(); ++i)
        context.segments.append({ segment_headers[i], segment_datas[i] });

    return {};
}

JBIG2ImageDecoderPlugin::JBIG2ImageDecoderPlugin()
{
    m_context = make<JBIG2LoadingContext>();
}

IntSize JBIG2ImageDecoderPlugin::size()
{
    return m_context->size;
}

bool JBIG2ImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return data.starts_with(id_string);
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JBIG2ImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JBIG2ImageDecoderPlugin()));
    TRY(decode_jbig2_header(*plugin->m_context, data));

    data = data.slice(sizeof(id_string) + sizeof(u8) + (plugin->m_context->number_of_pages.has_value() ? sizeof(u32) : 0));
    TRY(decode_segment_headers(*plugin->m_context, data));

    return plugin;
}

ErrorOr<ImageFrameDescriptor> JBIG2ImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    // FIXME: Use this for multi-page JBIG2 files?
    if (index != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid frame index");

    if (m_context->state == JBIG2LoadingContext::State::Error)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Decoding failed");

    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Draw the rest of the owl");
}

ErrorOr<ByteBuffer> JBIG2ImageDecoderPlugin::decode_embedded(Vector<ReadonlyBytes> data)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JBIG2ImageDecoderPlugin()));
    plugin->m_context->organization = Organization::Embedded;

    for (auto const& segment_data : data)
        TRY(decode_segment_headers(*plugin->m_context, segment_data));

    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode embedded JBIG2 yet");
}

}