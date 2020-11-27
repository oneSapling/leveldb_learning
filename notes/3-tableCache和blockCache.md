---
三、tablecache和blockcache
---

#(1)TableCache

tablecache cache的是 **多个sstable（不包含data block)** 。其key和value对应关系如下：

![](https://pic.downk.cc/item/5f82b2b81cd1bbb86b3919c9.png)

TableCache中的cache容量为990. 

```c++
// Number of open files that can be used by the DB.  
// You may need to increase this if your database has a large working set 
// (budget one open file per 2MB of working set).
int max_open_files = 1000;

const int kNumNonTableCacheFiles = 10;

static int TableCacheSize(const Options& sanitized_options) {
  // Reserve ten files or so for other uses and give the rest to TableCache.
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}
```

下面是整个TableCache:

```c++
class TableCache {
 public:
  TableCache(const std::string& dbname, const Options& options, int entries);
  ~TableCache();

  // Return an iterator for the specified file number (the corresponding
  // file length must be exactly "file_size" bytes).  If "tableptr" is
  // non-null, also sets "*tableptr" to point to the Table object
  // underlying the returned iterator, or to nullptr if no Table object
  // underlies the returned iterator.  The returned "*tableptr" object is owned
  // by the cache and should not be deleted, and is valid for as long as the
  // returned iterator is live.
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number,
                        uint64_t file_size, Table** tableptr = nullptr);

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(arg, found_key, found_value).
  Status Get(const ReadOptions& options, uint64_t file_number,
             uint64_t file_size, const Slice& k, void* arg,
             void (*handle_result)(void*, const Slice&, const Slice&));

  // Evict any entry for the specified file number
  void Evict(uint64_t file_number);

 private:
  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

  Env* const env_;
  const std::string dbname_;
  const Options& options_;
  Cache* cache_;
};
```

### 1.1 FindTable

FindTable是在cache中根据指定filenumnber,lookup到相关cache的handle。

如果找到了，直接返回该handle。

如果在cache中没找到，查找其handle,然后插入到cache中。

**此时的key是编码后的file_number. value是file_number对应的file指针以及打开后的sstable指针**

```c++
Status TableCache::FindTable(
                            uint64_t file_number, 
                            uint64_t file_size,
                            Cache::Handle** handle
){
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
   // 在cache寻找key对应的handle
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {	// 找不到
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
      // 根据fname打开file
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    if (s.ok()) {
        // 根据file打开table
      s = Table::Open(options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == nullptr);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
        // 插入到cache中
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}
```

<img src="https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/绘图文件-第 7 页.png" style="zoom:67%;" />

### 1.2 Get

```c++

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(arg, found_key, found_value).
Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&,
                                             const Slice&)) {
  Cache::Handle* handle = nullptr;
// 查找这个sstable的文件的habdle
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
      // 找到cache中的table
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
      // 在table内部寻找k，找到了对应data pair调用handle_result
    s = t->InternalGet(options, k, arg, handle_result);
    cache_->Release(handle);
  }
  return s;
}
```

下面看一下 InterNalGet() 函数

```c++
Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&,
                                                const Slice&)) {
  Status s;
   // 生成index迭代器
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);
  if (iiter->Valid()) {
      // 找到相关 data block
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {	// 应用bloom filter检查给定key是否在这个data block中
      // Not found
    } else {	
       // 可能在这个data block中
       // 将index的value转换为一个block iter
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        // 确实在这个data block中，调用handle_result函数（这里的arg可以是一个saver，用来保存找到的key和vlaue）
        (*handle_result)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  return s;
}

```

<img src="https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/绘图文件-第 8 页.png" style="zoom: 50%;" />

### 1.3 Evict

这个相对简单，直接在 cache_ 中 Erase 即可。

```c++
void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}
```

### 1.4 NewIterator

根据cache找到table，根据table返回iterator。

```c++
Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number, uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != nullptr) {
    *tableptr = table;
  }
  return result;
}
```

这里用到了TwoLevelIterator

```c++
Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}
```

**TwoLevelIterator**用index_iter和data_iter来访问数据。

**index_iter为index_block的 Block::Iter。**

**data_iter为 data block的Block::Iter。**

## (2)BlockCache

BlockCache用于cache sstable中的datablock。系统默认为8M. 

**key为 cahceid+data block的位置信息(offset)。**

**value 为 data block的block封装（解压后）。**

![](https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/绘图文件-第 9 页 (1).png)

### 2.1 BlockCache的初始化

blockcache的默认初始化在SanitizeOptions中：

```c++
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  ...
  if (result.block_cache == nullptr) {
      // block cache 的 capacity 为 8M
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}
```

### 2.2 Block Cache的读取与插入

	目前仅在Table::BlockReader看到使用。所以以这个函数来讲解。

```c++
// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
    // 获取 block_cache
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    BlockContents contents;
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(key, block, block->size(),
                                               &DeleteCachedBlock);
          }
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  ...
  return iter;
}
```

代码较长，分开来看，核心在

```c++
if (s.ok()) {
    BlockContents contents;
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {	// 在block cache中找到了block内容
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {		// 找不到block内容
         // 从table中读取block,注意此时blockcontents的内容已经是解压后的内容
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(key, block, block->size(),
                                               &DeleteCachedBlock);
          }
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }
```

如果在block_cache中找到了相关的内容，则直接返回相应的Block。找不到，则从table中读取相应的Block并放入到block_cache中。可以看到，block cahce的key为一个16字节的buffer, 前8个字节存放的cache_id, 后8个字节存放的data block所在sstable中的offset。value为一个block（解压后）。

> cache id的作用：
>
> // Return a new numeric id.  May be used by multiple clients who are
>
> // sharing the same cache to partition the key space.  Typically the
>
> // client will allocate a new id at startup and prepend the id to
>
> // its cache keys.

额外看一下Table是如何ReadBlock的。

```c++
s = ReadBlock(table->rep_->file, options, handle, &contents);
```

### 2.3 Table的ReadBlock

```c++
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result) {
  result->data = Slice();
  result->cachable = false;
  result->heap_allocated = false;

   // 读取完整的data block
  // Read the block contents as well as the type/crc footer.
  // See table_builder.cc for the code that built this structure.
  size_t n = static_cast<size_t>(handle.size());
  char* buf = new char[n + kBlockTrailerSize];
  Slice contents;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated block read");
  }

    // crc校验
  // Check the crc of the type and the block contents
  const char* data = contents.data();  // Pointer to where Read put the data
  if (options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      delete[] buf;
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
  }

   // 查看是否需要解压，如果需要解压，则解压数据
  switch (data[n]) {
    case kNoCompression:
      if (data != buf) {
        // File implementation gave us pointer to some other data.
        // Use it directly under the assumption that it will be live
        // while the file is open.
        delete[] buf;
        result->data = Slice(data, n);
        result->heap_allocated = false;
        result->cachable = false;  // Do not double-cache
      } else {
        result->data = Slice(buf, n);
        result->heap_allocated = true;
        result->cachable = true;
      }

      // Ok
      break;
    case kSnappyCompression: {
      size_t ulength = 0;
      if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
        delete[] buf;
        return Status::Corruption("corrupted compressed block contents");
      }
      char* ubuf = new char[ulength];
      if (!port::Snappy_Uncompress(data, n, ubuf)) {
        delete[] buf;
        delete[] ubuf;
        return Status::Corruption("corrupted compressed block contents");
      }
      delete[] buf;
      result->data = Slice(ubuf, ulength);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    }
    default:
      delete[] buf;
      return Status::Corruption("bad block type");
  }

  return Status::OK();
}

```