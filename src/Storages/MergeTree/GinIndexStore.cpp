// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)

#include <Storages/MergeTree/GinIndexStore.h>
#include <Columns/ColumnString.h>
#include <Common/FST.h>
#include <Compression/CompressionFactory.h>
#include <Compression/ICompressionCodec.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromVector.h>
#include <IO/WriteHelpers.h>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
};

const CompressionCodecPtr & GinIndexCompressionFactory::zstdCodec()
{
    static constexpr auto GIN_COMPRESSION_CODEC = "ZSTD";
    static constexpr auto GIN_COMPRESSION_LEVEL = 1;

    static auto codec = CompressionCodecFactory::instance().get(GIN_COMPRESSION_CODEC, GIN_COMPRESSION_LEVEL);
    return codec;
}

bool GinIndexPostingsBuilder::contains(UInt32 row_id) const
{
    return rowids.contains(row_id);
}

void GinIndexPostingsBuilder::add(UInt32 row_id)
{
    rowids.add(row_id);
}

UInt64 GinIndexPostingsBuilder::serialize(WriteBuffer & buffer)
{
    rowids.runOptimize();

    const UInt64 cardinality = rowids.cardinality();

    if (cardinality < MIN_SIZE_FOR_ROARING_ENCODING)
    {
        std::vector<UInt32> values(cardinality);
        rowids.toUint32Array(values.data());

        UInt64 header = (cardinality << 1) | ARRAY_CONTAINER_MASK;
        writeVarUInt(header, buffer);

        UInt64 written_bytes = getLengthOfVarUInt(header);
        for (const auto & value : values)
        {
            writeVarUInt(value, buffer);
            written_bytes += getLengthOfVarUInt(value);
        }

        return written_bytes;
    }

    const bool compress = cardinality >= ROARING_ENCODING_COMPRESSION_CARDINALITY_THRESHOLD;
    const UInt64 uncompressed_size = rowids.getSizeInBytes();

    std::vector<char> buf(uncompressed_size);
    rowids.write(buf.data());

    UInt64 header = uncompressed_size;
    if (compress)
    {
        Memory<> memory;
        const auto & codec = GinIndexCompressionFactory::zstdCodec();
        memory.resize(codec->getCompressedReserveSize(static_cast<UInt32>(uncompressed_size)));
        auto compressed_size = codec->compress(buf.data(), static_cast<UInt32>(uncompressed_size), memory.data());

        header = (header << 2) | (ROARING_COMPRESSED_MASK << 1) | ROARING_CONTAINER_MASK;

        writeVarUInt(header, buffer);
        writeVarUInt(compressed_size, buffer);
        buffer.write(memory.data(), compressed_size);

        return getLengthOfVarUInt(header) + getLengthOfVarUInt(compressed_size) + compressed_size;
    }
    else
    {
        header = (header << 2) | (ROARING_UNCOMPRESSED_MASK << 1) | ROARING_CONTAINER_MASK;

        writeVarUInt(header, buffer);
        buffer.write(buf.data(), uncompressed_size);

        return getLengthOfVarUInt(header) + uncompressed_size;
    }
}

GinIndexPostingsListPtr GinIndexPostingsBuilder::deserialize(ReadBuffer & buffer)
{
    /**
     * Header value maps into following states:
     * The lowest bit indicates if values are stored as an array or Roaring bitmap
     * In case of array container, the rest of the bits is the number of entries in the array.
     * In case of Roaring bitmap, the second lowest bit indicates if Roaring bitmap is compressed or uncompressed, the rest of the bits is the uncompressed size.
     */
    UInt64 header = 0;
    readVarUInt(header, buffer);

    if (header & ARRAY_CONTAINER_MASK) /// Array
    {
        UInt64 num_entries = (header >> 1);
        std::vector<UInt32> values(num_entries);
        for (size_t i = 0; i < num_entries; ++i)
            readVarUInt(values[i], buffer);

        GinIndexPostingsListPtr postings_list = std::make_shared<GinIndexPostingsList>();
        postings_list->addMany(values.size(), values.data());
        return postings_list;
    }
    else /// Roaring
    {
        header >>= 1;

        const bool compressed = header & ROARING_COMPRESSED_MASK;
        const UInt64 uncompressed_size = (header >> 1);
        if (compressed)
        {
            size_t compressed_size = 0;
            readVarUInt(compressed_size, buffer);
            std::vector<char> buf(compressed_size);
            buffer.readStrict(reinterpret_cast<char *>(buf.data()), compressed_size);

            Memory<> memory;
            memory.resize(uncompressed_size);
            const auto & codec = GinIndexCompressionFactory::zstdCodec();
            codec->decompress(buf.data(), static_cast<UInt32>(compressed_size), memory.data());

            return std::make_shared<GinIndexPostingsList>(GinIndexPostingsList::read(memory.data()));
        }
        else
        {
            /// Deserialize uncompressed roaring bitmap
            std::vector<char> buf(uncompressed_size);
            buffer.readStrict(buf.data(), uncompressed_size);
            return std::make_shared<GinIndexPostingsList>(GinIndexPostingsList::read(buf.data()));
        }
    }
}

GinIndexStore::GinIndexStore(const String & name_, DataPartStoragePtr storage_)
    : name(name_)
    , storage(storage_)
{
}

GinIndexStore::GinIndexStore(
    const String & name_,
    DataPartStoragePtr storage_,
    MutableDataPartStoragePtr data_part_storage_builder_,
    UInt64 segment_digestion_threshold_bytes_)
    : name(name_)
    , storage(storage_)
    , data_part_storage_builder(data_part_storage_builder_)
    , segment_digestion_threshold_bytes(segment_digestion_threshold_bytes_)
{
}

bool GinIndexStore::exists() const
{
    String segment_id_file_name = getName() + GIN_SEGMENT_ID_FILE_TYPE;
    return storage->existsFile(segment_id_file_name);
}

UInt32 GinIndexStore::getNextSegmentIDRange(size_t n)
{
    std::lock_guard guard(mutex);

    if (next_available_segment_id == 0)
        initSegmentId();

    UInt32 segment_id = next_available_segment_id;
    next_available_segment_id += n;
    return segment_id;
}

UInt32 GinIndexStore::getNextRowIDRange(size_t numIDs)
{
    UInt32 result = current_segment.next_row_id;
    current_segment.next_row_id += numIDs;
    return result;
}

UInt32 GinIndexStore::getNextSegmentID()
{
    return getNextSegmentIDRange(1);
}

namespace
{
GinIndexStore::Format getFormatVersion(uint8_t version)
{
    using FormatAsInt = std::underlying_type_t<GinIndexStore::Format>;
    switch (version)
    {
        case static_cast<FormatAsInt>(GinIndexStore::Format::v1):
            return GinIndexStore::Format::v1;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Text Index: segment ID file contains an unsupported version '{}'", version);
    }
}
}

UInt32 GinIndexStore::getNumOfSegments()
{
    if (cached_segment_num)
        return cached_segment_num;

    String segment_id_file_name = getName() + GIN_SEGMENT_ID_FILE_TYPE;
    if (!storage->existsFile(segment_id_file_name))
        return 0;

    UInt32 result = 0;
    {
        std::unique_ptr<DB::ReadBufferFromFileBase> istr = this->storage->readFile(segment_id_file_name, {}, std::nullopt, std::nullopt);

        uint8_t version = 0;
        readBinary(version, *istr);

        getFormatVersion(version);

        readVarUInt(result, *istr);
    }

    cached_segment_num = result - 1;
    return cached_segment_num;
}

GinIndexStore::Format GinIndexStore::getVersion()
{
    String segment_id_file_name = getName() + GIN_SEGMENT_ID_FILE_TYPE;
    if (!storage->existsFile(segment_id_file_name))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Text Index: segment ID file does not exist");

    std::unique_ptr<DB::ReadBufferFromFileBase> istr = this->storage->readFile(segment_id_file_name, {}, std::nullopt, std::nullopt);
    uint8_t version = 0;
    readBinary(version, *istr);
    return getFormatVersion(version);
}

bool GinIndexStore::needToWriteCurrentSegment() const
{
    /// segment_digestion_threshold_bytes != 0 means GinIndexStore splits the index data into separate segments.
    /// In case it's equal to 0 (zero), segment size is unlimited. Therefore, there will be a single segment.
    return (segment_digestion_threshold_bytes != UNLIMITED_SEGMENT_DIGESTION_THRESHOLD_BYTES) && (current_size > segment_digestion_threshold_bytes);
}

void GinIndexStore::finalize()
{
    if (!current_postings.empty())
    {
        writeSegment();
        writeSegmentId();
    }

    if (metadata_file_stream)
        metadata_file_stream->finalize();

    if (dict_file_stream)
        dict_file_stream->finalize();

    if (postings_file_stream)
        postings_file_stream->finalize();
}

void GinIndexStore::cancel() noexcept
{
    if (metadata_file_stream)
        metadata_file_stream->cancel();

    if (dict_file_stream)
        dict_file_stream->cancel();

    if (postings_file_stream)
        postings_file_stream->cancel();
}

void GinIndexStore::initSegmentId()
{
    String segment_id_file_name = getName() + GIN_SEGMENT_ID_FILE_TYPE;

    UInt32 segment_id;
    if (storage->existsFile(segment_id_file_name))
    {
        std::unique_ptr<DB::ReadBufferFromFileBase> istr = this->storage->readFile(segment_id_file_name, {}, std::nullopt, std::nullopt);

        uint8_t version = 0;
        readBinary(version, *istr);

        getFormatVersion(version);

        readVarUInt(segment_id, *istr);
    }
    else
        segment_id = 1;

    next_available_segment_id = segment_id;
}

void GinIndexStore::initFileStreams()
{
    String metadata_file_name = getName() + GIN_SEGMENT_METADATA_FILE_TYPE;
    String dict_file_name = getName() + GIN_DICTIONARY_FILE_TYPE;
    String postings_file_name = getName() + GIN_POSTINGS_FILE_TYPE;

    metadata_file_stream = data_part_storage_builder->writeFile(metadata_file_name, 4096, WriteMode::Append, {});
    dict_file_stream = data_part_storage_builder->writeFile(dict_file_name, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Append, {});
    postings_file_stream = data_part_storage_builder->writeFile(postings_file_name, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Append, {});
}

void GinIndexStore::writeSegmentId()
{
    String segment_id_file_name = getName() + GIN_SEGMENT_ID_FILE_TYPE;
    std::unique_ptr<DB::WriteBufferFromFileBase> ostr = this->data_part_storage_builder->writeFile(segment_id_file_name, 8, {});

    /// Write version
    writeChar(static_cast<char>(CURRENT_GIN_FILE_FORMAT_VERSION), *ostr);

    writeVarUInt(next_available_segment_id, *ostr);
    ostr->sync();
    ostr->finalize();
}

void GinIndexStore::writeSegment()
{
    if (metadata_file_stream == nullptr)
        initFileStreams();

    using TokenPostingsBuilderPair = std::pair<std::string_view, GinIndexPostingsBuilderPtr>;
    using TokenPostingsBuilderPairs = std::vector<TokenPostingsBuilderPair>;

    /// Write segment
    metadata_file_stream->write(reinterpret_cast<char *>(&current_segment), sizeof(GinIndexSegment));
    TokenPostingsBuilderPairs token_postings_list_pairs;
    token_postings_list_pairs.reserve(current_postings.size());

    for (const auto & [token, postings_list] : current_postings)
        token_postings_list_pairs.push_back({token, postings_list});

    /// Sort token-postings list pairs since all tokens have to be added in FST in sorted order
    std::sort(token_postings_list_pairs.begin(), token_postings_list_pairs.end(),
                    [](const TokenPostingsBuilderPair & x, const TokenPostingsBuilderPair & y)
                    {
                        return x.first < y.first;
                    });

    /// Write postings
    std::vector<UInt64> posting_list_byte_sizes(current_postings.size(), 0);

    for (size_t i = 0; const auto & [token, postings_list] : token_postings_list_pairs)
    {
        auto posting_list_byte_size = postings_list->serialize(*postings_file_stream);

        posting_list_byte_sizes[i] = posting_list_byte_size;
        i++;
        current_segment.postings_start_offset += posting_list_byte_size;
    }
    ///write item dictionary
    std::vector<UInt8> buffer;
    WriteBufferFromVector<std::vector<UInt8>> write_buf(buffer);
    FST::FstBuilder fst_builder(write_buf);

    UInt64 offset = 0;
    for (size_t i = 0; const auto & [token, postings_list] : token_postings_list_pairs)
    {
        fst_builder.add(token, offset);
        offset += posting_list_byte_sizes[i];
        i++;
    }

    fst_builder.build();
    write_buf.finalize();

    const size_t uncompressed_size = buffer.size();
    const bool compress_fst = uncompressed_size >= FST_SIZE_COMPRESSION_THRESHOLD;

    /// Header contains the uncompressed size and a single bit to indicate whether FST is compressed or uncompressed.
    UInt64 fst_size_header = (uncompressed_size << 1) | (compress_fst ? 0x1 : 0x0);
    /// Write FST size header
    writeVarUInt(fst_size_header, *dict_file_stream);
    current_segment.dict_start_offset += getLengthOfVarUInt(fst_size_header);

    if (compress_fst)
    {
        const auto & codec = GinIndexCompressionFactory::zstdCodec();
        Memory<> memory;
        memory.resize(codec->getCompressedReserveSize(static_cast<UInt32>(uncompressed_size)));
        auto compressed_size = codec->compress(reinterpret_cast<char *>(buffer.data()), uncompressed_size, memory.data());

        /// Write FST compressed size
        writeVarUInt(compressed_size, *dict_file_stream);
        current_segment.dict_start_offset += getLengthOfVarUInt(compressed_size);

        /// Write FST compressed blob
        dict_file_stream->write(memory.data(), compressed_size);
        current_segment.dict_start_offset += compressed_size;
    }
    else
    {
        /// Write FST uncompressed blob
        dict_file_stream->write(reinterpret_cast<char *>(buffer.data()), uncompressed_size);
        current_segment.dict_start_offset += uncompressed_size;
    }

    current_size = 0;
    current_postings.clear();
    current_segment.segment_id = getNextSegmentID();

    metadata_file_stream->sync();
    dict_file_stream->sync();
    postings_file_stream->sync();
}

GinIndexStoreDeserializer::GinIndexStoreDeserializer(const GinIndexStorePtr & store_)
    : store(store_)
{
    initFileStreams();
}

void GinIndexStoreDeserializer::initFileStreams()
{
    String metadata_file_name = store->getName() + GinIndexStore::GIN_SEGMENT_METADATA_FILE_TYPE;
    String dict_file_name = store->getName() + GinIndexStore::GIN_DICTIONARY_FILE_TYPE;
    String postings_file_name = store->getName() + GinIndexStore::GIN_POSTINGS_FILE_TYPE;

    metadata_file_stream = store->storage->readFile(metadata_file_name, {}, std::nullopt, std::nullopt);
    dict_file_stream = store->storage->readFile(dict_file_name, {}, std::nullopt, std::nullopt);
    postings_file_stream = store->storage->readFile(postings_file_name, {}, std::nullopt, std::nullopt);
}
void GinIndexStoreDeserializer::readSegments()
{
    UInt32 num_segments = store->getNumOfSegments();
    if (num_segments == 0)
        return;

    using GinIndexSegments = std::vector<GinIndexSegment>;
    GinIndexSegments segments (num_segments);

    assert(metadata_file_stream != nullptr);

    metadata_file_stream->readStrict(reinterpret_cast<char *>(segments.data()), num_segments * sizeof(GinIndexSegment));
    for (UInt32 i = 0; i < num_segments; ++i)
    {
        auto seg_id = segments[i].segment_id;
        auto seg_dict = std::make_shared<GinSegmentDictionary>();
        seg_dict->postings_start_offset = segments[i].postings_start_offset;
        seg_dict->dict_start_offset = segments[i].dict_start_offset;
        store->segment_dictionaries[seg_id] = seg_dict;
    }
}

void GinIndexStoreDeserializer::readSegmentDictionaries()
{
    for (UInt32 seg_index = 0; seg_index < store->getNumOfSegments(); ++seg_index)
        readSegmentDictionary(seg_index);
}

void GinIndexStoreDeserializer::readSegmentDictionary(UInt32 segment_id)
{
    /// Check validity of segment_id
    auto it = store->segment_dictionaries.find(segment_id);
    if (it == store->segment_dictionaries.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Invalid segment id {}", segment_id);

    /// Set file pointer of dictionary file
    assert(dict_file_stream != nullptr);
    dict_file_stream->seek(it->second->dict_start_offset, SEEK_SET);

    switch (auto version = store->getVersion(); version)
    {
        case GinIndexStore::Format::v1: {
            /// Read FST size header
            UInt64 fst_size_header;
            readVarUInt(fst_size_header, *dict_file_stream);

            size_t uncompressed_fst_size = fst_size_header >> 1;
            it->second->offsets.getData().clear();
            it->second->offsets.getData().resize(uncompressed_fst_size);
            if (fst_size_header & 0x1) /// FST is compressed
            {
                /// Read compressed FST size
                size_t compressed_fst_size = 0;
                readVarUInt(compressed_fst_size, *dict_file_stream);
                /// Read compressed FST blob
                std::vector<char> buf(compressed_fst_size);
                dict_file_stream->readStrict(buf.data(), compressed_fst_size);
                const auto & codec = DB::GinIndexCompressionFactory::zstdCodec();
                codec->decompress(
                    buf.data(), static_cast<UInt32>(compressed_fst_size), reinterpret_cast<char *>(it->second->offsets.getData().data()));
            }
            else
            {
                /// Read uncompressed FST blob
                dict_file_stream->readStrict(reinterpret_cast<char *>(it->second->offsets.getData().data()), uncompressed_fst_size);
            }
            break;
        }
    }
}

GinSegmentedPostingsListContainer GinIndexStoreDeserializer::readSegmentedPostingsLists(const String & term)
{
    assert(postings_file_stream != nullptr);

    GinSegmentedPostingsListContainer container;
    for (auto const & seg_dict : store->segment_dictionaries)
    {
        auto segment_id = seg_dict.first;

        auto [offset, found] = seg_dict.second->offsets.getOutput(term);
        if (!found)
            continue;

        // Set postings file pointer for reading postings list
        postings_file_stream->seek(seg_dict.second->postings_start_offset + offset, SEEK_SET);

        // Read posting list
        auto postings_list = GinIndexPostingsBuilder::deserialize(*postings_file_stream);
        container[segment_id] = postings_list;
    }
    return container;
}

GinPostingsCachePtr GinIndexStoreDeserializer::createPostingsCacheFromTerms(const std::vector<String> & terms)
{
    auto postings_cache = std::make_shared<GinPostingsCache>();
    for (const auto & term : terms)
    {
        // Make sure don't read for duplicated terms
        if (postings_cache->find(term) != postings_cache->end())
            continue;

        auto container = readSegmentedPostingsLists(term);
        (*postings_cache)[term] = container;
    }
    return postings_cache;
}

GinPostingsCachePtr PostingsCacheForStore::getPostings(const String & query_string) const
{
    auto it = cache.find(query_string);
    if (it == cache.end())
        return nullptr;
    return it->second;
}

GinIndexStoreFactory & GinIndexStoreFactory::instance()
{
    static GinIndexStoreFactory instance;
    return instance;
}

GinIndexStorePtr GinIndexStoreFactory::get(const String & name, DataPartStoragePtr storage)
{
    const String & part_path = storage->getRelativePath();
    String key = name + ":" + part_path;

    std::lock_guard lock(mutex);
    GinIndexStores::const_iterator it = stores.find(key);

    if (it == stores.end())
    {
        GinIndexStorePtr store = std::make_shared<GinIndexStore>(name, storage);
        if (!store->exists())
            return nullptr;

        GinIndexStoreDeserializer deserializer(store);
        deserializer.readSegments();
        deserializer.readSegmentDictionaries();

        stores[key] = store;

        return store;
    }
    return it->second;
}

void GinIndexStoreFactory::remove(const String & part_path)
{
    std::lock_guard lock(mutex);
    for (auto it = stores.begin(); it != stores.end();)
    {
        if (it->first.find(part_path) != String::npos)
            it = stores.erase(it);
        else
            ++it;
    }
}

bool isGinFile(const String & file_name)
{
    return file_name.ends_with(".gin_dict") || file_name.ends_with(".gin_post") || file_name.ends_with(".gin_seg") || file_name.ends_with(".gin_sid");
}

}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
