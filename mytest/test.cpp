//
// Created by raven on 9/15/20.
//
#include <iostream>
#include <ctype.h>
#include <sys/time.h>
#include <leveldb/env.h>
#include "../leveldb/include/leveldb/filter_policy.h"
#include "../leveldb/include/leveldb/db.h"
#include "../leveldb/include/leveldb/write_batch.h"

using namespace std;
using namespace leveldb;

void test1()
{
    leveldb::DB *db;
    leveldb::Options opts;
    opts.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(opts,"/home/zs/github/leveldb_learning/mytest/testdb",&db);

    /* write data test */
    status = db->Put(leveldb::WriteOptions(),"name","raven");
    /* Read data test*/
    string value;
    status = db->Get(leveldb::ReadOptions(),"name",&value);

    cout << value << endl;

    /* batch write test */
    leveldb::WriteBatch batch;
    batch.Delete("name");
    batch.Put("name0","raven0");
    batch.Put("name1","raven1");
    batch.Put("name2","raven2");
    batch.Put("name3","raven3");
    batch.Put("name4","raven4");
    batch.Put("name5","raven5");
    batch.Put("name6","raven6");
    batch.Put("name7","raven7");
    status = db->Write(leveldb::WriteOptions(), &batch);


    /* scan database */
    leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
    for(it->SeekToFirst(); it->Valid(); it->Next()){
        cout << it->key().ToString() <<":" << it->value().ToString() << "\n";
    }

    /* scan range [name3,name7) */
    for(it->Seek("name3"); it->Valid() && it->key().ToString() < "name8";it->Next()){
        cout << it->key().ToString() <<":" << it->value().ToString() << "\n";
    }

    /* write a large key value pair */
    string l_key = "large file";
    char buf[1024*100];
    string l_value(buf);
    leveldb::Status  s = db->Put(leveldb::WriteOptions(), l_key, l_value);

    delete db;
}

void test2()
{
    leveldb::DB *db;
    leveldb::Options opts;
    opts.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(opts,"/home/zs/github/leveldb_learning/mytest/testdb",&db);

    srand(time(0));
    string key;
    string value;
    char buf[100];
    for(unsigned int i = 0;i<=INT32_MAX;i++){
        fill(begin(buf),end(buf),'\0');
        sprintf(buf,"key%d",rand());
        key = buf;

        fill(begin(buf),end(buf),'\0');
        sprintf(buf,"value%d",rand());
        value = buf;
        db->Put(WriteOptions(),key,value);
    }

    delete db;
}


void test3()
{
    clock_t start,end;
    leveldb::DB *db;
    leveldb::Options opts;
    opts.create_if_missing = true;
    const FilterPolicy *pPolicy = NewBloomFilterPolicy(10);
    opts.filter_policy= pPolicy;
    opts.compression = kNoCompression;
    leveldb::Status status = leveldb::DB::Open(opts,"/home/zs/github/leveldb_learning/mytest/testdb",&db);

    /* key 2bytes value 200bytes */
    const int len = 3500;
    char key[6];
    char value[len];
    /*
    for(int i = 0;i<300000;i++){
        sprintf(key,"%d",i);
        db->Put(leveldb::WriteOptions(),key,Slice(value,len));
    }
    */
    string get_value;
    start=clock();
    printf("开始读取");
    for(int i = 0;i < 300000;i++){
        sprintf(key,"%d",i);
        const Status &s = db->Get(leveldb::ReadOptions(), key, &get_value);
        if(s.ok()){
            cout << i << " search success\n";
        }else {
            cout << i << " search failed\n";
        }
    }
    end = clock();
    delete db;
    printf("读取消耗时间=%f\n",(float)(end-start)*1000/CLOCKS_PER_SEC);
    /**
     * 2736.020996
     * 2581.082764ms
     */
}

double micro_time(){
    struct timeval tim;
    double ret;
    gettimeofday(&tim, NULL);
    ret = tim.tv_sec+(tim.tv_usec/1000000.0);
    return ret;
}

void sstTrack(){
    srand ( time(NULL) );
    DB *db ;
    Options op;
    op.create_if_missing = true;
    Status s = DB::Open(op,"/home/zs/github/leveldb_learning/mytest/testdb",&db);
    Env * env = Env::Default();
    WritableFile *file;
    env->NewWritableFile("/home/zs/github/leveldb_learning/mytest/bench.csv",&file);

    if(s.ok()){
        cout << "create successfully" << endl;

        WriteOptions wop;
        for(int j=0;j<300;++j){
            double start = micro_time();
            double cost;
            for(int i=0;i<10000;++i){
                char key[100];
                char value[100];
                sprintf(key,"%d_%d",i,rand());
                sprintf(value,"%d",rand());
                db->Put(wop,key,value);
            }
            cost = micro_time()-start;
            cout << "write successfully:" << j << ",costs "<<cost<<endl;
            // report the status
            {
                //output stats information
                string value;
                char buffer[40];
                for(int i=0;i<7;++i){
                    sprintf(buffer,"leveldb.num-files-at-level%d",i);
                    db->GetProperty(buffer,&value);
                    file->Append(value+",");
                }
                sprintf(buffer,"%f",cost);
                file->Append(buffer);
                file->Append("\n");
                file->Sync();
            }
        }
        cout << "write completed" << endl;
    }

    delete db;
    file->Close();
    delete file;
}

void blockTract(){
    DB *db ;
    Options op;
    op.create_if_missing = true;
    Status s = DB::Open(op,"/home/zs/github/leveldb_learning/mytest/testdb",&db);
    char buffer[40];
    string value;
    for(int i=0;i<7;++i){
        sprintf(buffer,"leveldb.num-files-at-level%d",i);
        db->GetProperty(buffer,&value);
    }
}

void readTest(){
    DB *db ;
    Options op;
    op.create_if_missing = true;
    Status s = DB::Open(op,"/home/zs/github/leveldb_learning/mytest/testdb",&db);
    const int len = 3500;
    char key[6];
    char value[len];
    /*
    for(int i = 0;i<300000;i++){
        sprintf(key,"%d",i);
        db->Put(leveldb::WriteOptions(),key,Slice(value,len));
    }
    */
    string get_value;
    // 将12写入到key中
    sprintf(key,"%d",12);
    db->Get(leveldb::ReadOptions(), key, &get_value);
    cout << get_value;
}

int main()
{
    //test2();
    //test3();
    //sstTrack();
    //blockTract();
    /*
    Version* current_;        // == dummy_versions_.prev_
    std::vector<FileMetaData*> files_[config::kNumLevels];
    // 获取level1的全部sstable
    std::vector<FileMetaData*> d = current_->files_[1];
    // 获取第一个ssstab的元数据
    FileMetaData* sstable =  d[0];
    State state;
    bool a = Match(void* ,1 , sstable);
    */
    readTest();
    return 0;
}

