---
二 . 缓存的设计
---

leveldb是一种对写优化的kv存储系统，主要是利用的HDD硬盘的顺序写远高于随机写的特性，来优化存储系统的写如性能。

但是读性能有所下降，提高读性能最常用的方法就是cache。

为了充分利用局部性原理，提高读性能，leveldb自己也设计了一个Cache结构，内部采用LRU替换策略。

leveldb中的cache系统分为TableCache和BlockCache

leveldb中的cache主要包含以下类：
```
- LRUHandle -- 数据节点
- HandleTable -- HashTable
- LRUCache
- ShardedLRUCache
```

事实上到了第三个数据结构LRUCache，LRU的缓存管理数据结构已经实现了，之所以引入第四个数据结构，就是因为减少竞争。

因为多线程访问需要加锁，为了减少竞争，提升效率.

ShardedLRUCache内部有**16个LRUCach**e，查找key的时候，先计算属于哪一个LRUCache，然后在相应的LRUCache中上锁查找,这种分段的hash结构很好的提高了并发能力。

```c++
class ShardedLRUCache : public Cache {  
 private:  
  LRUCache shard_[kNumShards];  
  ...
}
```

因此，读懂缓存管理策略的关键在前三个数据结构。

LevelDB的Cache管理，维护有2个双向链表和一个哈希表。

哈希表是非常容易理解的。如何确定一个key值到底存不存在，如果存在如何快速获取key值对应的value值。这活，哈希表是比较适合的。

注意，我们都知道，hash表存在一个重要的问题，就是碰撞，有可能多个不同的键值hash之后值相同，解决碰撞的一个重要思路是链表，将hash之后计算的key相同的元素链入同一个表头对应的链表。

note：（ 这里使用链表结构来解决hash碰撞，其实也是有问题的。那就是，在链表中的数据量变的很大的时候，每次查询可能都要遍历整个链表来找到需要的数据，阿里巴巴团队在fast19上的一篇文章HotRing通过热指针的方式，
来解决hash冲突之后每一次都要查询链表中全部数据的问题。- 后续详细学习一下HotRing的设计原理。）

可是我们并不满意这种速度，LevelDB做了进一步的优化，即及时扩大hash桶的个数，尽可能地不会发生碰撞。因此LevelDB自己实现了一个hash表，即HandleTable数据结构。

说句题外话，我不太喜欢数据结构的命名方式，比如HandleTable，命名就是个HashTable，如果出现Hash会好理解很多。

这个名字还就算了，LRUHandle这个名字更是让人摸不到头脑，明明就是一个数据节点，如果名字中出现Node，整个代码都会好理解很多。好了吐槽结束，看下HandleTable的数据结构：

## 1. HandleTable

HandleTable在本质上就是一个HashTable，只是leveldb做了优化，通过尽早的拓展桶，来达到减少碰撞的目的。

成员变量：

```c++
private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  uint32_t length_;	// buckets个数
  uint32_t elems_;  // 当前插入的elem个数
  LRUHandle** list_;	// 二级指针，每个一级指针指向一个桶
```

### Insert

```c++
  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;	// 替换操作
    if (old == nullptr) {
      ++elems_;
      if (elems_ > length_) {	// 尽可能保证一个bucket只有一个elem
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        Resize();
      }
    }
    return old;
  }
```

insert函数首先在当前hashtable中尝试找到与插入key具有相同key的entry，即旧entry：

1.如果存在旧entry，则将旧的覆盖为新的节点。

2.如果不存在旧的entry，则添加一个新的节点。

同时可能需要resize hash， resize的条件为“当前插入的节点数比hashtable 的 bucket数大”，

这样做的目的是尽量保证每个bucket下只有一个entry，这样search时间能够保证在O(1)。

### Resize

```c++
 void Resize() {
    uint32_t new_length = 4;	// 保证新容量是4的整数倍
    while (new_length < elems_) {	// 2倍扩容
      new_length *= 2;
    }
     // 新hashtable
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    // 移动旧hashtable中的元素到新hashtable
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
		// 下面3行代码的结果是，如果旧hashtable的一个bucket的多个node，都重新链接到了这个新hashtable的同一个bucket，则这些node将会反序连接
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
     // 删除旧表
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
```

<img src="https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/leveldb源码阅读-copy-第 23 页.png" style="zoom:33%;" />

### Remove

```c++
  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;	// remove掉当前节点，并指向下一个节点
      --elems_;
    }
    return result;
  }

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];		// 二级指针
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }
```

注意，这里remove的entry并没有free掉。entry的free在后文的LRUCache中。

<img src="https://pic.downk.cc/item/5f8421811cd1bbb86be7cd0d.png" style="zoom:33%;" />



### 总结

leveldb的hashtable其实就是一个数组+链表的hashtable，只不过rehash操作做了优化，从而加快search的效率。

<img src="https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/绘图文件-第 3 页.png" style="zoom:50%;" />

## 2. LRUHandle

```c++
struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void* value);
  LRUHandle* next_hash;
  LRUHandle* next;		// 双链表的next
  LRUHandle* prev;		// 双链表的prev
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  bool in_cache;     // Whether entry is in the cache.
  uint32_t refs;     // References, including cache reference, if present. 引用计数
  uint32_t hash;     // Hash of key(); used for fast sharding and comparisons
  char key_data[1];  // Beginning of key, ！！ 占位符，这里放在结构体的最后且只有一个字节是有目的的，后面说到

  Slice key() const {
    // next_ is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    assert(next != this);

    return Slice(key_data, key_length);
  }
};
```

LRUHandle表示一个cache节点。其中next_hash字段用在hashtable中，表明相同bucekt下的下一个节点。

## 3. LRUCache

### 总layout：

<img src="https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/绘图文件-第 4 页.png" style="zoom:50%;" />

HandleTable是用来找到某个cache entry的。但是无法实现LRU算法，现在来说一下实际的LRUCache。在LRUCache的实现里维护了两个链表：

```c++
  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  LRUHandle lru_ GUARDED_BY(mutex_);	// 冷数据的链表

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  LRUHandle in_use_ GUARDED_BY(mutex_);		// 热数据链表

  HandleTable table_ GUARDED_BY(mutex_); // 之前讲过
```

注意在lru_链表中， lru.prev代表最新entry，lru.next代表最旧entry。

一个cache entry在上述两条链中的其中一个，通过 Ref() and Unref()调用，一个cache entry在两条链表之间move。

### 1. Ref

Ref将一个cache 节点，从lru_链表插入到in\_user\_链表

```c++
void LRUCache::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  // 插入到最新位置,list head处
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}
```

### 2. UnRef

如果client不再引用一条cache entry， 则会进行UnRef，当一个cache ref=0时，则删除这个entry，当cache ref = 1时，则从热链(in\_user_)迁移到冷链(lru\_)。

```c++
void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);	// 这里free
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}
```

### 3. Insert（evict）

cache是有容量大小限制的，当插入的cache entry达到一定数量时，需要根据LRU算法剔除旧cache。这部分实现的入口在Insert函数：

```c++

Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  LRUHandle* e =		// !!! 这里解释了上面为什么申请了一个字节的 key_data并且放在最后
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  if (capacity_ > 0) {
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    LRU_Append(&in_use_, e);
    usage_ += charge;
    FinishErase(table_.Insert(e));
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    // next is read by key() in an assert, so it must be initialized
    e->next = nullptr;
  }
    
   // ！！！超过容量，需要剔除旧cache lru_.next存放的是最旧的cache entry
  while (usage_ > capacity_ && lru_.next != &lru_) {	// 移除旧cache entry，直到当前usage 小于等于 capacity。 或者最旧的entry已经是lru_
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
     // 从hashtable中移除，从cache中删除。
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}
```

这里有3个地方需要说明：

1. LRUHandler中为什么申请了一个字节的key_data:

   ```c++
   LRUHandle* e =		// !!! 这里解释了上面为什么申请了一个字节的 key_data并且放在最后
         reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
   ```

   <img src="https://cdn.jsdelivr.net/gh/ravenxrz/PicBed/img/绘图文件.png" style="zoom:33%;" />

2. 新增一个cache entry到cache中：

```c++
   if (capacity_ > 0) {
       e->refs++;  // for the cache's reference.
       e->in_cache = true;
       LRU_Append(&in_use_, e);	// 插入到热链中 in_user_
       usage_ += charge;
       FinishErase(table_.Insert(e));	// 如果是更新，需要删除旧entry
     } 
```

```c++
     LRUHandle* Insert(LRUHandle* h) {
       LRUHandle** ptr = FindPointer(h->key(), h->hash);
       LRUHandle* old = *ptr;
       h->next_hash = (old == nullptr ? nullptr : old->next_hash);
       *ptr = h;
       if (old == nullptr) {
         ++elems_;
         if (elems_ > length_) {
           // Since each cache entry is fairly large, we aim for a small
           // average linked list length (<= 1).
           Resize();
         }
       }
       return old;
     }
```

   Insert在Update的情况下，会返回旧的cache entry. 在FinishErase函数调用中决定该cache entry是删除还是移动到lru_链中：

```c++
   // If e != nullptr, finish removing *e from the cache; it has already been
   // removed from the hash table.  Return whether e != nullptr.
   bool LRUCache::FinishErase(LRUHandle* e) {
     if (e != nullptr) {
       assert(e->in_cache);
       LRU_Remove(e);
       e->in_cache = false;
       usage_ -= e->charge;
       Unref(e);
     }
     return e != nullptr;
   }
```

3. LRU evict：

```c++
      // ！！！超过容量，需要剔除旧cache lru_.next存放的是最旧的cache entry
     while (usage_ > capacity_ && lru_.next != &lru_) {	// 移除旧cache entry，直到当前usage 小于等于 capacity。 或者最旧的entry已经是lru_头，即cache为空
       LRUHandle* old = lru_.next;
       assert(old->refs == 1);
        // 从hashtable中移除，从cache中删除。
       bool erased = FinishErase(table_.Remove(old->key(), old->hash));
       if (!erased) {  // to avoid unused variable when compiled NDEBUG
         assert(erased);
       }
     }
```

LRU是如何体现的？

回到插入一个entry到lru_中：

```c++
LRU_Append(&lru_, e);

void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}
```

每次插入都是向list head的前面插入一个新节点，作为最新节点。所以有：

![](https://pic.downk.cc/item/5f84428f1cd1bbb86b050fa0.png)

## 4. ShardedLRUCache

前面说了ShardedLRUCache是为了减少多线程的竞争延迟而设计的。在SharedLRUCache中有16个LRUCache。

```c++
static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

class ShardedLRUCache : public Cache {
 private:
  LRUCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};
```

如何做到减少竞争带来的延迟的？现在有多个cache，对于每个插入的key，做一次hash，然后取hash结果的高4位作为cache id，用于选择此次用哪个cache来缓存数据。
这样就避免了只使用一个cache时，每次插入都要加锁。
