// DataVault - Complete Final Version
// Includes: Full FileSystem + AES-128 + AVL Tree + HTTP Server (cpp-httplib)
//
// HOW TO BUILD:
//   1. Download httplib.h (single header, no install needed):
//      curl -O https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
//
//   2. Compile:
//      Linux/Mac:  g++ proj.cpp -o datavault -lpthread -std=c++17
//      Windows:    g++ proj.cpp -o datavault.exe -lws2_32 -std=c++17
//
//   3. Run:  ./datavault
//      Then open datavault.html in your browser.

#include <iostream>
#include <string>
#include <functional>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "httplib.h"

using namespace std;

// -------------------- Console Colors (kept for server logs) --------------------
#define RESET   "\033[0m"
#define GREEN   "\033[32m"
#define CYAN    "\033[36m"
#define YELLOW  "\033[33m"

static void ensure_storage_dir() {
#ifdef _WIN32
    _mkdir("storage");
#else
    mkdir("storage", 0755);
#endif
}

// -------------------- Exceptions --------------------
class FileSystemException : public exception {
private:
    string message;
public:
    FileSystemException(const string& msg) : message(msg) {}
    const char* what() const noexcept override { return message.c_str(); }
};

// -------------------- Permissions --------------------
enum Permission {
    PERM_READ          = 0x1,
    PERM_WRITE         = 0x2,
    PERM_EXECUTE       = 0x4,
    PERM_MODIFY_ACCESS = 0x8
};

class User;
class FileMetadata;
class AVLTreeNode;

struct LoggedInUserNode {
    User* user;
    LoggedInUserNode* next;
    LoggedInUserNode(User* u) : user(u), next(NULL) {}
};

// -------------------- Stack --------------------
template <typename T>
class Stack {
private:
    struct Node { T data; Node* next; Node(T val) : data(val), next(NULL) {} };
    Node* top;
public:
    Stack() : top(NULL) {}
    ~Stack() { while (!isEmpty()) pop(); }
    void push(T val) { Node* n = new Node(val); n->next = top; top = n; }
    T pop() {
        if (!top) return T();
        Node* temp = top; T data = temp->data; top = top->next; delete temp; return data;
    }
    bool isEmpty() { return top == NULL; }
};

// -------------------- Queue --------------------
template <typename T>
class Queue {
private:
    struct Node { T data; Node* next; Node(T val) : data(val), next(NULL) {} };
    Node* front; Node* rear; int count;
public:
    Queue() : front(NULL), rear(NULL), count(0) {}
    Queue(const Queue& other) : front(NULL), rear(NULL), count(0) {
        Node* cur = other.front;
        while (cur) { enqueue(cur->data); cur = cur->next; }
    }
    ~Queue() { while (!isEmpty()) dequeue(); }
    void enqueue(T val) {
        Node* n = new Node(val);
        if (!rear) front = rear = n;
        else { rear->next = n; rear = n; }
        count++;
    }
    T dequeue() {
        if (!front) return T();
        Node* temp = front; T data = front->data;
        front = front->next;
        if (!front) rear = NULL;
        delete temp; count--; return data;
    }
    bool isEmpty() const { return front == NULL; }
    int size() const { return count; }
};

// -------------------- Hash Table --------------------
template <typename K, typename V>
class HashTable {
private:
    struct Entry { K key; V value; Entry* next; Entry(K k, V v) : key(k), value(v), next(NULL) {} };
    static const int TABLE_SIZE = 256;
    Entry** table;
    int hash(const K& key) const {
        unsigned long h = 5381;
        for (size_t i = 0; i < key.length(); i++) h = ((h << 5) + h) + (unsigned char)key[i];
        return (int)(h % TABLE_SIZE);
    }
public:
    HashTable() { table = new Entry*[TABLE_SIZE](); }
    ~HashTable() {
        for (int i = 0; i < TABLE_SIZE; i++) {
            Entry* e = table[i];
            while (e) { Entry* t = e; e = e->next; delete t; }
        }
        delete[] table;
    }
    void insert(const K& key, V value) {
        int idx = hash(key); Entry* e = table[idx];
        if (!e) { table[idx] = new Entry(key, value); return; }
        Entry* prev = NULL;
        while (e) { if (e->key == key) { e->value = value; return; } prev = e; e = e->next; }
        prev->next = new Entry(key, value);
    }
    bool get(const K& key, V& value) const {
        int idx = hash(key); Entry* e = table[idx];
        while (e) { if (e->key == key) { value = e->value; return true; } e = e->next; }
        return false;
    }
    void remove(const K& key) {
        int idx = hash(key); Entry* e = table[idx]; Entry* prev = NULL;
        while (e) {
            if (e->key == key) {
                if (prev) prev->next = e->next; else table[idx] = e->next;
                delete e; return;
            }
            prev = e; e = e->next;
        }
    }
    void iterate(std::function<void(const K&, V)> cb) const {
        for (int i = 0; i < TABLE_SIZE; i++) {
            Entry* e = table[i];
            while (e) { cb(e->key, e->value); e = e->next; }
        }
    }
};

// -------------------- Compressor --------------------
class Compressor {
public:
    static string compressRLE(const string& input) {
        string output; int count = 1;
        for (size_t i = 1; i <= input.size(); i++) {
            if (i < input.size() && input[i] == input[i-1]) count++;
            else { output += to_string(count) + input[i-1]; count = 1; }
        }
        return output;
    }
    static string decompressRLE(const string& input) {
        string output; int i = 0;
        while (i < (int)input.size()) {
            string numStr;
            while (i < (int)input.size() && isdigit((unsigned char)input[i])) numStr += input[i++];
            if (i >= (int)input.size()) break;
            int count = numStr.empty() ? 1 : stoi(numStr);
            output += string(count, input[i++]);
        }
        return output;
    }
    static string compressDict(const string& input) {
        string output = input; size_t pos;
        while ((pos = output.find("the")) != string::npos) output.replace(pos, 3, "1");
        while ((pos = output.find("and")) != string::npos) output.replace(pos, 3, "2");
        return output;
    }
    static string decompressDict(const string& input) {
        string output = input; size_t pos;
        while ((pos = output.find("1")) != string::npos) output.replace(pos, 1, "the");
        while ((pos = output.find("2")) != string::npos) output.replace(pos, 1, "and");
        return output;
    }
};

// -------------------- AES-128 --------------------
static const unsigned char sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const unsigned char inv_sbox[256] = {
  0x52,0x09,0x6A,0xD5,0x30,0x36,0xA5,0x38,0xBF,0x40,0xA3,0x9E,0x81,0xF3,0xD7,0xFB,
  0x7C,0xE3,0x39,0x82,0x9B,0x2F,0xFF,0x87,0x34,0x8E,0x43,0x44,0xC4,0xDE,0xE9,0xCB,
  0x54,0x7B,0x94,0x32,0xA6,0xC2,0x23,0x3D,0xEE,0x4C,0x95,0x0B,0x42,0xFA,0xC3,0x4E,
  0x08,0x2E,0xA1,0x66,0x28,0xD9,0x24,0xB2,0x76,0x5B,0xA2,0x49,0x6D,0x8B,0xD1,0x25,
  0x72,0xF8,0xF6,0x64,0x86,0x68,0x98,0x16,0xD4,0xA4,0x5C,0xCC,0x5D,0x65,0xB6,0x92,
  0x6C,0x70,0x48,0x50,0xFD,0xED,0xB9,0xDA,0x5E,0x15,0x46,0x57,0xA7,0x8D,0x9D,0x84,
  0x90,0xD8,0xAB,0x00,0x8C,0xBC,0xD3,0x0A,0xF7,0xE4,0x58,0x05,0xB8,0xB3,0x45,0x06,
  0xD0,0x2C,0x1E,0x8F,0xCA,0x3F,0x0F,0x02,0xC1,0xAF,0xBD,0x03,0x01,0x13,0x8A,0x6B,
  0x3A,0x91,0x11,0x41,0x4F,0x67,0xDC,0xEA,0x97,0xF2,0xCF,0xCE,0xF0,0xB4,0xE6,0x73,
  0x96,0xAC,0x74,0x22,0xE7,0xAD,0x35,0x85,0xE2,0xF9,0x37,0xE8,0x1C,0x75,0xDF,0x6E,
  0x47,0xF1,0x1A,0x71,0x1D,0x29,0xC5,0x89,0x6F,0xB7,0x62,0x0E,0xAA,0x18,0xBE,0x1B,
  0xFC,0x56,0x3E,0x4B,0xC6,0xD2,0x79,0x20,0x9A,0xDB,0xC0,0xFE,0x78,0xCD,0x5A,0xF4,
  0x1F,0xDD,0xA8,0x33,0x88,0x07,0xC7,0x31,0xB1,0x12,0x10,0x59,0x27,0x80,0xEC,0x5F,
  0x60,0x51,0x7F,0xA9,0x19,0xB5,0x4A,0x0D,0x2D,0xE5,0x7A,0x9F,0x93,0xC9,0x9C,0xEF,
  0xA0,0xE0,0x3B,0x4D,0xAE,0x2A,0xF5,0xB0,0xC8,0xEB,0xBB,0x3C,0x83,0x53,0x99,0x61,
  0x17,0x2B,0x04,0x7E,0xBA,0x77,0xD6,0x26,0xE1,0x69,0x14,0x63,0x55,0x21,0x0C,0x7D
};
static const unsigned char Rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36};

static void KeyExpansion(const unsigned char* key, unsigned char* rk) {
    for (int i = 0; i < 16; i++) rk[i] = key[i];
    int bg = 16, ri = 1; unsigned char tmp[4];
    while (bg < 176) {
        for (int i = 0; i < 4; i++) tmp[i] = rk[bg-4+i];
        if (bg % 16 == 0) {
            unsigned char t = tmp[0]; tmp[0]=tmp[1]; tmp[1]=tmp[2]; tmp[2]=tmp[3]; tmp[3]=t;
            for (int i=0;i<4;i++) tmp[i]=sbox[tmp[i]];
            tmp[0] ^= Rcon[ri++];
        }
        for (int i=0;i<4;i++) rk[bg]=rk[bg-16]^tmp[i], bg++;
    }
}
static void AddRoundKey(unsigned char s[16], const unsigned char* rk) { for(int i=0;i<16;i++) s[i]^=rk[i]; }
static void SubBytes(unsigned char s[16])    { for(int i=0;i<16;i++) s[i]=sbox[s[i]]; }
static void InvSubBytes(unsigned char s[16]) { for(int i=0;i<16;i++) s[i]=inv_sbox[s[i]]; }
static void ShiftRows(unsigned char s[16]) {
    unsigned char t[16];
    t[0]=s[0]; t[1]=s[5]; t[2]=s[10]; t[3]=s[15];
    t[4]=s[4]; t[5]=s[9]; t[6]=s[14]; t[7]=s[3];
    t[8]=s[8]; t[9]=s[13]; t[10]=s[2]; t[11]=s[7];
    t[12]=s[12]; t[13]=s[1]; t[14]=s[6]; t[15]=s[11];
    for(int i=0;i<16;i++) s[i]=t[i];
}
static void InvShiftRows(unsigned char s[16]) {
    unsigned char t[16];
    t[0]=s[0]; t[1]=s[13]; t[2]=s[10]; t[3]=s[7];
    t[4]=s[4]; t[5]=s[1]; t[6]=s[14]; t[7]=s[11];
    t[8]=s[8]; t[9]=s[5]; t[10]=s[2]; t[11]=s[15];
    t[12]=s[12]; t[13]=s[9]; t[14]=s[6]; t[15]=s[3];
    for(int i=0;i<16;i++) s[i]=t[i];
}
static unsigned char mul(unsigned char a, unsigned char b) {
    unsigned char r=0, tmp=a;
    for(int i=0;i<8;i++){if(b&1)r^=tmp; unsigned char h=(unsigned char)(tmp&0x80); tmp<<=1; if(h)tmp^=0x1b; b>>=1;}
    return r;
}
static void MixColumns(unsigned char s[16]) {
    for(int i=0;i<4;i++){
        int x=4*i;
        unsigned char a0=s[x],a1=s[x+1],a2=s[x+2],a3=s[x+3];
        s[x]  =(unsigned char)(mul(2,a0)^mul(3,a1)^a2^a3);
        s[x+1]=(unsigned char)(a0^mul(2,a1)^mul(3,a2)^a3);
        s[x+2]=(unsigned char)(a0^a1^mul(2,a2)^mul(3,a3));
        s[x+3]=(unsigned char)(mul(3,a0)^a1^a2^mul(2,a3));
    }
}
static void InvMixColumns(unsigned char s[16]) {
    for(int i=0;i<4;i++){
        int x=4*i;
        unsigned char a0=s[x],a1=s[x+1],a2=s[x+2],a3=s[x+3];
        s[x]  =(unsigned char)(mul(0x0e,a0)^mul(0x0b,a1)^mul(0x0d,a2)^mul(0x09,a3));
        s[x+1]=(unsigned char)(mul(0x09,a0)^mul(0x0e,a1)^mul(0x0b,a2)^mul(0x0d,a3));
        s[x+2]=(unsigned char)(mul(0x0d,a0)^mul(0x09,a1)^mul(0x0e,a2)^mul(0x0b,a3));
        s[x+3]=(unsigned char)(mul(0x0b,a0)^mul(0x0d,a1)^mul(0x09,a2)^mul(0x0e,a3));
    }
}
static void AES_encrypt_block(const unsigned char in[16], unsigned char out[16], const unsigned char rk[176]) {
    unsigned char s[16]; for(int i=0;i<16;i++) s[i]=in[i];
    AddRoundKey(s,rk);
    for(int r=1;r<=9;r++){SubBytes(s);ShiftRows(s);MixColumns(s);AddRoundKey(s,rk+16*r);}
    SubBytes(s); ShiftRows(s); AddRoundKey(s,rk+160);
    for(int i=0;i<16;i++) out[i]=s[i];
}
static void AES_decrypt_block(const unsigned char in[16], unsigned char out[16], const unsigned char rk[176]) {
    unsigned char s[16]; for(int i=0;i<16;i++) s[i]=in[i];
    AddRoundKey(s,rk+160);
    for(int r=9;r>=1;r--){InvShiftRows(s);InvSubBytes(s);AddRoundKey(s,rk+16*r);InvMixColumns(s);}
    InvShiftRows(s); InvSubBytes(s); AddRoundKey(s,rk);
    for(int i=0;i<16;i++) out[i]=s[i];
}
static void pkcs7_pad(const unsigned char* in, int in_len, unsigned char** out, int* out_len) {
    int pad = 16 - (in_len % 16); *out_len = in_len + pad;
    *out = (unsigned char*)malloc(*out_len);
    memcpy(*out, in, in_len);
    for(int i=0;i<pad;i++) (*out)[in_len+i]=(unsigned char)pad;
}
static bool pkcs7_unpad(unsigned char* in, int in_len, int* out_len) {
    if(in_len<=0){*out_len=0;return false;}
    unsigned char pad=in[in_len-1];
    if(pad<=0||pad>16){*out_len=in_len;return false;}
    for(int i=0;i<pad;i++) if(in[in_len-1-i]!=pad){*out_len=in_len;return false;}
    *out_len=in_len-pad; return true;
}
static bool AES128_ECB_encrypt_buffer(const unsigned char* key, const unsigned char* in, int in_len, unsigned char** out, int* out_len) {
    unsigned char rk[176]; KeyExpansion(key,rk);
    unsigned char* padded=NULL; int pl=0;
    pkcs7_pad(in,in_len,&padded,&pl);
    *out_len=pl; *out=(unsigned char*)malloc(pl);
    for(int off=0;off<pl;off+=16) AES_encrypt_block(padded+off,*out+off,rk);
    free(padded); return true;
}
static bool AES128_ECB_decrypt_buffer(const unsigned char* key, const unsigned char* in, int in_len, unsigned char** out, int* out_len) {
    if(in_len%16!=0){*out_len=0;*out=NULL;return false;}
    unsigned char rk[176]; KeyExpansion(key,rk);
    unsigned char* tmp=(unsigned char*)malloc(in_len);
    for(int off=0;off<in_len;off+=16) AES_decrypt_block(in+off,tmp+off,rk);
    int pl=0; pkcs7_unpad(tmp,in_len,&pl);
    *out_len=pl; *out=(unsigned char*)malloc(pl+1);
    memcpy(*out,tmp,pl); (*out)[pl]=0; free(tmp); return true;
}

// -------------------- File Version --------------------
struct FileVersion {
    string content; time_t timestamp;
    FileVersion* prev; FileVersion* next;
    bool compressed; string compressionType; string diskPath;
    FileVersion(string c, time_t t) : content(c), timestamp(t), prev(NULL), next(NULL), compressed(false), compressionType(""), diskPath("") {}
};

// -------------------- File Metadata --------------------
class FileMetadata {
public:
    string name, type, owner;
    size_t size;
    FileVersion* versions;
    time_t lastAccessed;
    int accessCount;

    FileMetadata(string n, string t, string o)
        : name(n), type(t), owner(o), versions(NULL), lastAccessed(time(NULL)), accessCount(0), size(0) {
        addVersion("");
    }
    // Constructor for loading from disk — does NOT create a blank version
    FileMetadata(string n, string t, string o, bool /*loadMode*/)
        : name(n), type(t), owner(o), versions(NULL), lastAccessed(time(NULL)), accessCount(0), size(0) {}
    ~FileMetadata() {
        FileVersion* cur = versions;
        while (cur) { FileVersion* t = cur; cur = cur->prev; delete t; }
    }

    void writeEncryptedToDisk(const string& plaintext) {
        ensure_storage_dir();
        unsigned char key[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
        unsigned char* enc=NULL; int enc_len=0;
        AES128_ECB_encrypt_buffer(key,(const unsigned char*)plaintext.c_str(),(int)plaintext.length(),&enc,&enc_len);
        string clean=name;
        for(size_t i=0;i<clean.length();i++) if(clean[i]==' ') clean[i]='_';
        stringstream ss; ss<<"storage/"<<owner<<"_"<<clean<<"_"<<(int)time(NULL)<<".dat";
        string path=ss.str();
        ofstream ofs(path.c_str(),ios::binary);
        if(ofs.fail()){ if(enc) free(enc); throw FileSystemException("Failed to open: "+path); }
        ofs.write((const char*)enc,enc_len); ofs.close();
        if(enc) free(enc);
        FileVersion* nv=new FileVersion(plaintext,time(NULL));
        nv->diskPath=path;
        if(!versions) versions=nv;
        else { nv->prev=versions; versions->next=nv; versions=nv; }
        size=plaintext.size(); lastAccessed=time(NULL); accessCount=0;
    }

    string readDecryptedFromDisk(FileVersion* version) {
        if(!version) return "";
        if(version->diskPath.empty()) return version->content;
        ifstream ifs(version->diskPath.c_str(),ios::binary|ios::ate);
        if(ifs.fail()) return "";
        streamsize sz=ifs.tellg(); ifs.seekg(0,ios::beg);
        unsigned char* buf=(unsigned char*)malloc((size_t)sz);
        ifs.read((char*)buf,sz); ifs.close();
        unsigned char key[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
        unsigned char* dec=NULL; int dec_len=0;
        bool ok=AES128_ECB_decrypt_buffer(key,buf,(int)sz,&dec,&dec_len);
        free(buf);
        if(!ok){ if(dec) free(dec); return ""; }
        string result; if(dec_len>0) result.assign((char*)dec,dec_len);
        if(dec) free(dec); return result;
    }

    void addVersion(string content) {
        try { writeEncryptedToDisk(content); }
        catch(const FileSystemException&) {
            FileVersion* nv=new FileVersion(content,time(NULL));
            if(!versions) versions=nv;
            else { nv->prev=versions; versions->next=nv; versions=nv; }
            size=content.size();
        }
    }

    void compress(const string& method="RLE") {
        if(versions){
            string pt=readDecryptedFromDisk(versions);
            pt=(method=="RLE")?Compressor::compressRLE(pt):Compressor::compressDict(pt);
            writeEncryptedToDisk(pt);
            versions->compressed=true; versions->compressionType=method; size=pt.size();
        }
    }
    void decompress() {
        if(versions&&versions->compressed){
            string pt=readDecryptedFromDisk(versions);
            pt=(versions->compressionType=="RLE")?Compressor::decompressRLE(pt):Compressor::decompressDict(pt);
            writeEncryptedToDisk(pt);
            versions->compressed=false; versions->compressionType=""; size=pt.size();
        }
    }
};

// -------------------- AVL Tree --------------------
class AVLTreeNode {
public:
    string name; bool isDirectory;
    AVLTreeNode* parent; AVLTreeNode* left; AVLTreeNode* right;
    // Directory hierarchy — separate from AVL BST pointers
    AVLTreeNode* dirParent;
    AVLTreeNode* dirFirstChild;  // first child directory
    AVLTreeNode* dirNextSibling; // next sibling in parent's child list
    HashTable<string,FileMetadata*> files; int height;

    AVLTreeNode(string n,bool dir,AVLTreeNode* p=NULL)
        : name(n),isDirectory(dir),parent(p),left(NULL),right(NULL),
          dirParent(p),dirFirstChild(NULL),dirNextSibling(NULL),height(1){}
    ~AVLTreeNode(){delete left; delete right;}

    // Register this node as a child of dirP in the directory hierarchy
    void addDirChild(AVLTreeNode* child){
        child->dirParent=this;
        child->dirNextSibling=dirFirstChild;
        dirFirstChild=child;
    }

    int getBalance(){ return (left?left->height:0)-(right?right->height:0); }
    void updateHeight(){ height=1+max(left?left->height:0,right?right->height:0); }

    AVLTreeNode* rotateRight(){
        AVLTreeNode* nr=left; left=nr->right; nr->right=this;
        if(left) left->parent=this; nr->parent=parent; parent=nr;
        updateHeight(); nr->updateHeight(); return nr;
    }
    AVLTreeNode* rotateLeft(){
        AVLTreeNode* nr=right; right=nr->left; nr->left=this;
        if(right) right->parent=this; nr->parent=parent; parent=nr;
        updateHeight(); nr->updateHeight(); return nr;
    }
    AVLTreeNode* balance(){
        updateHeight(); int b=getBalance();
        if(b>1){ if(left->getBalance()<0) left=left->rotateLeft(); return rotateRight(); }
        if(b<-1){ if(right->getBalance()>0) right=right->rotateRight(); return rotateLeft(); }
        return this;
    }
    AVLTreeNode* insert(AVLTreeNode* node){
        if(node->name<name){ left=left?left->insert(node):node; left->parent=this; }
        else { right=right?right->insert(node):node; right->parent=this; }
        return balance();
    }
    AVLTreeNode* find(const string& n){
        if(n==name) return this;
        if(n<name) return left?left->find(n):NULL;
        return right?right->find(n):NULL;
    }
};

// -------------------- Cloud Sync --------------------
class SyncTask {
public:
    string filePath,action; time_t timestamp;
    SyncTask(const string& p,const string& a):filePath(p),action(a),timestamp(time(NULL)){}
};

class CloudSync {
    Queue<SyncTask*> syncQueue; bool online; thread* syncThread; bool running;
public:
    CloudSync():online(false),running(false),syncThread(NULL){}
    ~CloudSync(){stopSync();}
    void startSync(){ running=true; syncThread=new thread(&CloudSync::syncLoop,this); }
    void stopSync(){
        running=false;
        if(syncThread&&syncThread->joinable()){syncThread->join();delete syncThread;syncThread=NULL;}
    }
    void addSyncTask(const string& path,const string& action){ SyncTask* t=new SyncTask(path,action); syncQueue.enqueue(t); }
    void syncLoop(){
        while(running){
            if(!syncQueue.isEmpty()&&checkConnection()){
                SyncTask* t=syncQueue.dequeue();
                if(t){ processTask(t); delete t; }
            }
            this_thread::sleep_for(chrono::seconds(3));
        }
    }
    bool checkConnection(){ online=(rand()%4)!=0; return online; }
    void processTask(SyncTask* t){ /* cloud processing placeholder */ (void)t; }
    bool isOnline() const { return online; }
};

// -------------------- User --------------------
class User {
public:
    string username,password,securityQuestion,securityAnswer;
    time_t lastLogin,lastLogout; bool isLoggedIn;
    LoggedInUserNode* loginNode; int permissions;

    User():username(""),password(""),securityQuestion(""),securityAnswer(""),
           lastLogin(0),lastLogout(0),isLoggedIn(false),loginNode(NULL),permissions(PERM_READ){}
    User(string un,string pw,string q,string a)
        :username(un),password(pw),securityQuestion(q),securityAnswer(a),
         lastLogin(0),lastLogout(0),isLoggedIn(false),loginNode(NULL),permissions(PERM_READ){}

    void setRole(const string& role){
        permissions=0;
        if(role=="admin")  permissions=PERM_READ|PERM_WRITE|PERM_EXECUTE|PERM_MODIFY_ACCESS;
        else if(role=="editor") permissions=PERM_READ|PERM_WRITE;
        else permissions=PERM_READ;
    }
    bool hasPermission(Permission p) const { return (permissions&p)==p; }
    string getRole() const {
        if(hasPermission(PERM_MODIFY_ACCESS)) return "admin";
        if(hasPermission(PERM_WRITE)) return "editor";
        return "viewer";
    }
};

// -------------------- User Graph --------------------
class UserGraph {
private:
    struct Connection { User* user; string filename; Connection* next; Connection(User* u,const string& f=""):user(u),filename(f),next(NULL){} };
    struct UserNode {
        User* user; Connection* connections; Connection* sharedFiles;
        UserNode(User* u):user(u),connections(NULL),sharedFiles(NULL){}
        ~UserNode(){
            Connection* c=connections; while(c){Connection* t=c;c=c->next;delete t;}
            c=sharedFiles; while(c){Connection* t=c;c=c->next;delete t;}
        }
    };
    HashTable<string,UserNode*> userMap;
    UserNode* getUserNode(const string& un){ UserNode* n; return userMap.get(un,n)?n:NULL; }
public:
    void addUser(User* u){ UserNode* t; if(!userMap.get(u->username,t)) userMap.insert(u->username,new UserNode(u)); }
    bool addConnection(const string& u1,const string& u2){
        UserNode* n1=getUserNode(u1); UserNode* n2=getUserNode(u2);
        if(!n1||!n2) return false;
        Connection* c=n1->connections;
        while(c){ if(c->user->username==u2) return true; c=c->next; }
        Connection* nc1=new Connection(n2->user); nc1->next=n1->connections; n1->connections=nc1;
        Connection* nc2=new Connection(n1->user); nc2->next=n2->connections; n2->connections=nc2;
        return true;
    }
    bool shareFile(const string& sender,const string& receiver,const string& filename){
        UserNode* sn=getUserNode(sender); UserNode* rn=getUserNode(receiver);
        if(!sn||!rn) return false;
        bool connected=false; Connection* c=sn->connections;
        while(c){ if(c->user->username==receiver){connected=true;break;} c=c->next; }
        if(!connected) return false;
        Connection* sh=rn->sharedFiles;
        while(sh){ if(sh->user->username==sender&&sh->filename==filename) return true; sh=sh->next; }
        Connection* ns=new Connection(sn->user,filename); ns->next=rn->sharedFiles; rn->sharedFiles=ns;
        return true;
    }
    void getSharedFiles(const string& un, Queue<string>& result){
        UserNode* n=getUserNode(un); if(!n) return;
        Connection* c=n->sharedFiles;
        // Return as "sender|filename" so callers can parse sender and filename separately
        while(c){ result.enqueue(c->user->username+"|"+c->filename); c=c->next; }
    }
    void getConnections(const string& un,Queue<string>& result){
        UserNode* n=getUserNode(un); if(!n) return;
        Connection* c=n->connections;
        while(c){ result.enqueue(c->user->username); c=c->next; }
    }
    bool isConnected(const string& u1,const string& u2){
        UserNode* n=getUserNode(u1); if(!n) return false;
        Connection* c=n->connections;
        while(c){ if(c->user->username==u2) return true; c=c->next; }
        return false;
    }
};

// -------------------- FileSystem --------------------
class FileSystem {
private:
    AVLTreeNode* root; AVLTreeNode* currentDir;
    HashTable<string, AVLTreeNode*> userCurrentDir;
    HashTable<string, AVLTreeNode*> homeDirs; // username -> home AVLTreeNode* (never changes after creation)
    User* currentUser;
    HashTable<string,User*> users;
    HashTable<string, Stack<pair<FileMetadata*,string>>*> userRecycleBin;
    HashTable<string, Queue<string>*> userRecentFiles;
    const int MAX_RECENT_FILES=10;
    UserGraph userGraph;
    LoggedInUserNode* loggedInUsersHead; int loggedInUsersCount;
    CloudSync cloudSync;
    int adminCount;
    const string USERS_FILE="users.dat";
    const string OTP_FILE="OTP.txt";
    const string FILES_FILE="files.dat";
    const string CONN_FILE="connections.dat";

    // ---- Persist file metadata ----
    void saveAllFiles(){
        ofstream ofs(FILES_FILE.c_str(),ios::trunc); if(ofs.fail()) return;
        homeDirs.iterate([&](const string& dirOwner, AVLTreeNode* homeNode){
            // Save all directories under this home (including empty ones)
            Stack<AVLTreeNode*> dirStk; dirStk.push(homeNode);
            while(!dirStk.isEmpty()){
                AVLTreeNode* node=dirStk.pop();
                // Build path relative to homeNode
                string dirpath; AVLTreeNode* q=node;
                while(q&&q!=homeNode){dirpath="/"+q->name+dirpath;q=q->dirParent;}
                if(dirpath.empty()) dirpath="/";
                // Save a DIR marker so empty dirs are preserved
                ofs<<"DIR|"<<dirOwner<<"|"<<dirpath<<"\n";
                // Save files in this dir
                node->files.iterate([&](const string& fname, FileMetadata* fm){
                    string diskPath = fm->versions ? fm->versions->diskPath : "";
                    bool comp = fm->versions ? fm->versions->compressed : false;
                    string compType = fm->versions ? fm->versions->compressionType : "";
                    ofs<<dirOwner<<"|"<<dirpath<<"|"<<fname<<"|"<<fm->type<<"|"<<fm->owner<<"|"<<diskPath<<"|"<<(comp?"1":"0")<<"|"<<compType<<"\n";
                });
                // Push children
                AVLTreeNode* child=node->dirFirstChild;
                while(child){dirStk.push(child);child=child->dirNextSibling;}
            }
        });
        ofs.close();
    }

    void loadFilesFromFile(){
        ifstream ifs(FILES_FILE.c_str()); if(ifs.fail()) return;
        string line;
        while(getline(ifs,line)){
            if(line.empty()) continue;
            if(line.substr(0,4)=="DIR|"){
                // DIR|owner|dirpath — restore directory structure
                size_t p1=line.find('|',4); if(p1==string::npos) continue;
                size_t p2=line.find('|',p1+1); if(p2==string::npos) continue;
                string dirOwner=line.substr(4,p1-4);
                string dirpath =line.substr(p1+1,p2-p1-1);
                if(dirOwner.empty()) continue;
                AVLTreeNode* homeDir=getOrCreateHomeDir(dirOwner);
                if(dirpath=="/") continue; // home dir already created
                // Navigate/create path
                AVLTreeNode* parent=homeDir;
                size_t start=1;
                while(start<dirpath.size()){
                    size_t end=dirpath.find('/',start);
                    string seg=(end==string::npos)?dirpath.substr(start):dirpath.substr(start,end-start);
                    if(!seg.empty()){
                        AVLTreeNode* found=NULL;
                        AVLTreeNode* c=parent->dirFirstChild;
                        while(c){if(c->name==seg){found=c;break;}c=c->dirNextSibling;}
                        if(!found){
                            found=new AVLTreeNode(seg,true,NULL);
                            parent->addDirChild(found);
                            root=root->insert(found);
                            root->parent=NULL;
                        }
                        parent=found;
                    }
                    start=(end==string::npos)?dirpath.size():end+1;
                }
                continue;
            }
            // File line: owner|dirpath|filename|type|fileowner|diskPath|compressed|compressionType
            size_t p0=line.find('|'); if(p0==string::npos) continue;
            size_t p1=line.find('|',p0+1); if(p1==string::npos) continue;
            size_t p2=line.find('|',p1+1); if(p2==string::npos) continue;
            size_t p3=line.find('|',p2+1); if(p3==string::npos) continue;
            size_t p4=line.find('|',p3+1); if(p4==string::npos) continue;
            size_t p5=line.find('|',p4+1); if(p5==string::npos) continue;
            size_t p6=line.find('|',p5+1); if(p6==string::npos) continue;
            string dirOwner=line.substr(0,p0);
            string dirpath =line.substr(p0+1,p1-p0-1);
            string fname   =line.substr(p1+1,p2-p1-1);
            string ftype   =line.substr(p2+1,p3-p2-1);
            string owner   =line.substr(p3+1,p4-p3-1);
            string dpath   =line.substr(p4+1,p5-p4-1);
            bool   comp    =line.substr(p5+1,p6-p5-1)=="1";
            string ctype   =line.substr(p6+1);
            if(fname.empty()||dirOwner.empty()) continue;
            AVLTreeNode* homeDir=getOrCreateHomeDir(dirOwner);
            AVLTreeNode* dir=homeDir;
            if(dirpath!="/"){
                size_t start=1;
                AVLTreeNode* parent=homeDir;
                while(start<dirpath.size()){
                    size_t end=dirpath.find('/',start);
                    string seg=(end==string::npos)?dirpath.substr(start):dirpath.substr(start,end-start);
                    if(!seg.empty()){
                        AVLTreeNode* found=NULL;
                        AVLTreeNode* c=parent->dirFirstChild;
                        while(c){if(c->name==seg){found=c;break;}c=c->dirNextSibling;}
                        if(!found){
                            found=new AVLTreeNode(seg,true,NULL);
                            parent->addDirChild(found);
                            root=root->insert(found);
                            root->parent=NULL;
                        }
                        parent=found;
                    }
                    start=(end==string::npos)?dirpath.size():end+1;
                }
                dir=parent;
            }
            FileMetadata* ex; if(dir->files.get(fname,ex)) continue;
            FileMetadata* fm=new FileMetadata(fname,ftype,owner,true);
            FileVersion* fv=new FileVersion("",time(NULL));
            fv->diskPath=dpath; fv->compressed=comp; fv->compressionType=ctype;
            fm->versions=fv; fm->size=0;
            dir->files.insert(fname,fm);
        }
        ifs.close();
    }

    // Active OTP for current login attempt
    string pendingOtpUser;
    string pendingOtp;

    AVLTreeNode* findNode(string path){
        if(path=="/") return root;
        AVLTreeNode* tmp=root;
        size_t start=1,end;
        while((end=path.find('/',start))!=string::npos){
            string dn=path.substr(start,end-start);
            tmp=tmp->find(dn);
            if(!tmp||!tmp->isDirectory) return NULL;
            start=end+1;
        }
        return tmp->find(path.substr(start));
    }

    void clearLoggedInUsers(){
        LoggedInUserNode* cur=loggedInUsersHead;
        while(cur){LoggedInUserNode* t=cur;cur=cur->next;delete t;}
        loggedInUsersHead=NULL; loggedInUsersCount=0;
    }

    void updateRecentFiles(const string& filename){
        if(!currentUser) return;
        AVLTreeNode* homeDir=getOrCreateHomeDir(currentUser->username);
        string fp=getCurrentPath(homeDir); if(fp=="/") fp+=filename; else fp+="/"+filename;
        // Get or create this user's recent files queue
        Queue<string>* rq=NULL;
        if(!userRecentFiles.get(currentUser->username,rq)){
            rq=new Queue<string>(); userRecentFiles.insert(currentUser->username,rq);
        }
        Queue<string> temp; bool found=false;
        while(!rq->isEmpty()){ string f=rq->dequeue(); if(f==fp) found=true; else temp.enqueue(f); }
        while(!temp.isEmpty()) rq->enqueue(temp.dequeue());
        if(!found) rq->enqueue(fp);
        while(rq->size()>MAX_RECENT_FILES) rq->dequeue();
    }

    string getCurrentPath(AVLTreeNode* homeDir=NULL){
        if(!currentDir) return "/";
        if(homeDir&&currentDir==homeDir) return "/";
        string path; AVLTreeNode* n=currentDir;
        while(n&&n!=root&&n!=homeDir){ path="/"+n->name+path; n=n->dirParent; }
        return path.empty()?"/":path;
    }

    bool isValidName(const string& name){ if(name.empty()) return false; for(char c:name) if(c=='/') return false; return true; }
    bool checkAccess(User* user,FileMetadata* file,Permission req){ if(file->owner==user->username) return true; return user->hasPermission(req); }

    int countAdmins(){ int c=0; users.iterate([&](const string&,User* u){ if(u->hasPermission(PERM_MODIFY_ACCESS)) c++; }); return c; }

    void loadUsersFromFile(){
        ifstream ifs(USERS_FILE.c_str()); if(ifs.fail()) return;
        string line;
        while(getline(ifs,line)){
            if(line.empty()) continue;
            size_t p1=line.find('|'); if(p1==string::npos) continue;
            size_t p2=line.find('|',p1+1); if(p2==string::npos) continue;
            size_t p3=line.find('|',p2+1); if(p3==string::npos) continue;
            size_t p4=line.find('|',p3+1); if(p4==string::npos) continue;
            string un=line.substr(0,p1),pw=line.substr(p1+1,p2-p1-1),
                   q=line.substr(p2+1,p3-p2-1),a=line.substr(p3+1,p4-p3-1),role=line.substr(p4+1);
            if(un.empty()) continue;
            User* ex; if(users.get(un,ex)) continue;
            User* u=new User(un,pw,q,a);
            setRoleInternal(u,role); users.insert(un,u); userGraph.addUser(u);
        }
        ifs.close();
    }

    void saveAllUsers(){
        ofstream ofs(USERS_FILE.c_str(),ios::trunc); if(ofs.fail()) return;
        users.iterate([&](const string&,User* u){
            ofs<<u->username<<"|"<<u->password<<"|"<<u->securityQuestion<<"|"<<u->securityAnswer<<"|"<<u->getRole()<<"\n";
        });
        ofs.close();
    }

    void saveConnections(){
        ofstream ofs(CONN_FILE.c_str(),ios::trunc); if(ofs.fail()) return;
        users.iterate([&](const string& un, User*){
            Queue<string> conns; userGraph.getConnections(un,conns);
            while(!conns.isEmpty()){
                string other=conns.dequeue();
                if(un<other) ofs<<"CONN|"<<un<<"|"<<other<<"\n";
            }
        });
        // Save shares — record is "SHARE|sender|receiver|filename:dirpath"
        users.iterate([&](const string& un, User*){
            vector<SharedFileInfo> shares; getSharedFilesAPI(un, shares);
            for(auto& s:shares){
                ofs<<"SHARE|"<<s.sender<<"|"<<un<<"|"<<s.filename<<":"<<s.dirpath<<"\n";
            }
        });
        ofs.close();
    }

    void loadConnections(){
        ifstream ifs(CONN_FILE.c_str()); if(ifs.fail()) return;
        string line;
        while(getline(ifs,line)){
            if(line.empty()) continue;
            if(line.substr(0,5)=="CONN|"){
                size_t p1=line.find('|',5); if(p1==string::npos) continue;
                size_t p2=line.find('|',p1+1);
                string u1=line.substr(5,p1-5);
                string u2=(p2==string::npos)?line.substr(p1+1):line.substr(p1+1,p2-p1-1);
                if(u1.empty()||u2.empty()) continue;
                User* a; User* b;
                if(users.get(u1,a)&&users.get(u2,b))
                    userGraph.addConnection(u1,u2);
            } else if(line.substr(0,6)=="SHARE|"){
                // SHARE|sender|receiver|filename:dirpath
                size_t p1=line.find('|',6); if(p1==string::npos) continue;
                size_t p2=line.find('|',p1+1); if(p2==string::npos) continue;
                string sender  =line.substr(6,p1-6);
                string receiver=line.substr(p1+1,p2-p1-1);
                string record  =line.substr(p2+1); // "filename:dirpath"
                if(sender.empty()||receiver.empty()||record.empty()) continue;
                User* a; User* b;
                if(users.get(sender,a)&&users.get(receiver,b))
                    userGraph.shareFile(sender,receiver,record);
            }
        }
        ifs.close();
    }

    void setRoleInternal(User* u,const string& role){
        string r=role; for(char& c:r) if(c>='A'&&c<='Z') c=c+('a'-'A'); u->setRole(r);
    }

    string generateOTP(const string& username){
        srand((unsigned int)time(NULL)^(unsigned int)(uintptr_t)this);
        int otp=(rand()%900000)+100000;
        stringstream ss; ss<<otp;
        ofstream ofs(OTP_FILE.c_str());
        if(!ofs.fail()){ ofs<<"Username: "<<username<<"\nOTP: "<<ss.str()<<"\nGenerated at: "<<(int)time(NULL)<<"\n"; ofs.close(); }
        return ss.str();
    }

public:
    FileSystem():root(new AVLTreeNode("root",true)),currentDir(root),currentUser(NULL),
                 loggedInUsersHead(NULL),loggedInUsersCount(0),adminCount(0){
        cloudSync.startSync(); loadUsersFromFile(); ensure_storage_dir(); loadFilesFromFile(); loadConnections();
    }
    ~FileSystem(){ clearLoggedInUsers(); delete root; cloudSync.stopSync(); }

    // ---- Auth ----
    bool signupAPI(const string& un_raw,const string& pw,const string& q,const string& a,const string& role,string& errOut){
        // Trim whitespace from username
        string un = un_raw;
        while(!un.empty() && (un.front()==' '||un.front()=='\t'||un.front()=='\r'||un.front()=='\n')) un.erase(un.begin());
        while(!un.empty() && (un.back()==' '||un.back()=='\t'||un.back()=='\r'||un.back()=='\n')) un.pop_back();
        string suffix="@nu.edu.pk";
        if(un.length()<suffix.length()||un.substr(un.length()-suffix.length())!=suffix){ errOut="Only @nu.edu.pk emails can register"; return false; }
        if(pw.length()<6){ errOut="Password must be at least 6 characters"; return false; }
        if(!isValidName(un)){ errOut="Invalid username"; return false; }
        User* tmp; if(users.get(un,tmp)){ errOut="User already exists"; return false; }
        if(q.empty()||a.empty()){ errOut="Security Q&A cannot be empty"; return false; }
        User* nu=new User(un,pw,q,a);
        users.insert(un,nu); userGraph.addUser(nu);
        if(countAdmins()==0) setRoleInternal(nu,"admin"); else setRoleInternal(nu,role);
        saveAllUsers(); return true;
    }

    // Step 1: validate credentials, generate OTP
    bool loginStep1(const string& un_raw,const string& pw,string& errOut){
        string un = un_raw;
        while(!un.empty() && (un.front()==' '||un.front()=='\t'||un.front()=='\r'||un.front()=='\n')) un.erase(un.begin());
        while(!un.empty() && (un.back()==' '||un.back()=='\t'||un.back()=='\r'||un.back()=='\n')) un.pop_back();
        if(un.empty()||pw.empty()){ errOut="Username and password required"; return false; }
        User* user; if(!users.get(un,user)){ errOut="User not found"; return false; }
        if(user->password!=pw){ errOut="Invalid password"; return false; }
        if(user->isLoggedIn){ errOut="User already logged in"; return false; }
        pendingOtpUser=un;
        pendingOtp=generateOTP(un);
        return true;
    }

    // Step 2: verify OTP, complete login
    bool loginStep2(const string& un,const string& otp,string& errOut){
        if(un!=pendingOtpUser){ errOut="No pending login for this user"; return false; }
        if(otp!=pendingOtp){ errOut="Invalid OTP"; return false; }
        User* user; if(!users.get(un,user)){ errOut="User not found"; return false; }
        LoggedInUserNode* nn=new LoggedInUserNode(user);
        nn->next=loggedInUsersHead; loggedInUsersHead=nn;
        user->loginNode=nn; currentUser=user;
        user->lastLogin=time(NULL); user->isLoggedIn=true; loggedInUsersCount++;
        pendingOtpUser=""; pendingOtp="";
        return true;
    }

    bool logoutAPI(const string& un,string& errOut){
        User* user; if(!users.get(un,user)){ errOut="User not found"; return false; }
        if(!user->isLoggedIn){ errOut="User not logged in"; return false; }
        user->lastLogout=time(NULL); user->isLoggedIn=false; loggedInUsersCount--;
        LoggedInUserNode* prev=NULL,*cur=loggedInUsersHead;
        while(cur){ if(cur->user==user){ if(prev) prev->next=cur->next; else loggedInUsersHead=cur->next; delete cur; break; } prev=cur; cur=cur->next; }
        if(currentUser==user) currentUser=loggedInUsersHead?loggedInUsersHead->user:NULL;
        // Reset saved directory so next login starts at home
        userCurrentDir.remove(un);
        return true;
    }

    bool resetPasswordAPI(const string& un,const string& ans,const string& np,string& errOut){
        User* user; if(!users.get(un,user)){ errOut="User not found"; return false; }
        if(ans!=user->securityAnswer){ errOut="Incorrect security answer"; return false; }
        if(np.length()<6){ errOut="Password must be at least 6 characters"; return false; }
        user->password=np; saveAllUsers(); return true;
    }

    bool setUserRoleAPI(const string& callerUn,const string& targetUn,const string& role,string& errOut){
        User* caller; if(!users.get(callerUn,caller)){ errOut="Caller not found"; return false; }
        if(!caller->hasPermission(PERM_MODIFY_ACCESS)){ errOut="No permission"; return false; }
        User* target; if(!users.get(targetUn,target)){ errOut="Target not found"; return false; }
        string r=role; for(char& c:r) if(c>='A'&&c<='Z') c=c+('a'-'A');
        if(r!="admin"&&r!="editor"&&r!="viewer"){ errOut="Invalid role"; return false; }
        if(caller->username==targetUn&&r!="admin"&&countAdmins()==1){ errOut="Cannot demote only admin"; return false; }
        target->setRole(r); saveAllUsers(); return true;
    }

    // ---- Directory & File ops ----
    // All return bool + errOut for clean API responses

    // Get or create a home directory for a user — stored as depth-1 nodes (root's direct subtree top level)
    // We identify home dirs by searching all nodes and checking depth from root == 1
    // Find a direct child of root (depth==1) by name
    AVLTreeNode* getOrCreateHomeDir(const string& un){
        AVLTreeNode* home=NULL;
        if(homeDirs.get(un,home)) return home;
        // Create home dir node and register it permanently
        home=new AVLTreeNode(un,true,NULL);
        root=root->insert(home);
        root->parent=NULL;
        homeDirs.insert(un,home);
        saveAllFiles();
        return home;
    }

    bool setCurrentUser(const string& un){
        User* u; if(!users.get(un,u)) return false;
        currentUser=u;
        AVLTreeNode* home=getOrCreateHomeDir(un);
        // Restore this user's last known directory, defaulting to their home
        AVLTreeNode* saved; 
        if(userCurrentDir.get(un,saved)) currentDir=saved;
        else currentDir=home;
        return true;
    }

    bool cdAPI(const string& path,string& errOut,string& newPath){
        if(!currentUser){ errOut="Not logged in"; return false; }
        AVLTreeNode* homeDir=getOrCreateHomeDir(currentUser->username);
        if(path=="/"){ currentDir=homeDir; userCurrentDir.insert(currentUser->username,currentDir); newPath="/"; return true; }
        if(path==".."){
            if(currentDir==homeDir||currentDir==root){ currentDir=homeDir; userCurrentDir.insert(currentUser->username,currentDir); newPath="/"; return true; }
            currentDir=currentDir->dirParent?currentDir->dirParent:homeDir;
            // Make sure we don't go above home
            AVLTreeNode* check=currentDir; bool inHome=false;
            while(check){ if(check==homeDir){inHome=true;break;} check=check->dirParent; }
            if(!inHome) currentDir=homeDir;
            userCurrentDir.insert(currentUser->username,currentDir);
            newPath=getCurrentPath(homeDir);
            return true;
        }
        AVLTreeNode* target=NULL;
        if(path[0]=='/'){
            if(path=="/"){currentDir=homeDir;userCurrentDir.insert(currentUser->username,currentDir);newPath="/";return true;}
            // Walk segments from homeDir using dir hierarchy
            AVLTreeNode* tmp=homeDir; size_t start=1;
            bool ok=true;
            while(start<path.size()){
                size_t end=path.find('/',start);
                string seg=(end==string::npos)?path.substr(start):path.substr(start,end-start);
                if(!seg.empty()){
                    AVLTreeNode* found=NULL;
                    AVLTreeNode* c=tmp->dirFirstChild;
                    while(c){ if(c->name==seg&&c->isDirectory){found=c;break;} c=c->dirNextSibling; }
                    if(!found){ok=false;break;}
                    tmp=found;
                }
                start=(end==string::npos)?path.size():end+1;
            }
            if(ok) target=tmp;
        } else {
            // Relative — search direct dir-children of currentDir
            AVLTreeNode* c=currentDir->dirFirstChild;
            while(c){ if(c->name==path&&c->isDirectory){target=c;break;} c=c->dirNextSibling; }
        }
        if(target&&target->isDirectory){
            currentDir=target;
            newPath=getCurrentPath(homeDir);
            userCurrentDir.insert(currentUser->username,currentDir);
            return true;
        }
        errOut="Directory not found: "+path; return false;
    }

    // List current directory — returns lists of file/dir info
    struct FileInfo { string name,type,owner; bool compressed; };
    struct DirInfo  { string name; };

    void listAPI(vector<FileInfo>& files,vector<DirInfo>& dirs){
        if(!currentUser) return;
        AVLTreeNode* homeDir=getOrCreateHomeDir(currentUser->username);
        // List direct dir-children of currentDir that are within user's home subtree
        AVLTreeNode* child=currentDir->dirFirstChild;
        while(child){
            // Verify it's within user's home
            AVLTreeNode* check=child; bool inHome=false;
            while(check){ if(check==homeDir){inHome=true;break;} check=check->dirParent; }
            if(inHome) dirs.push_back({child->name});
            child=child->dirNextSibling;
        }
        // Files stored in currentDir's hash table
        currentDir->files.iterate([&](const string& name, FileMetadata* fm){
            bool comp = fm->versions && fm->versions->compressed;
            files.push_back({name, fm->type, fm->owner, comp});
        });
    }

    bool mkdirAPI(const string& name,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        if(!currentUser->hasPermission(PERM_WRITE)){ errOut="Viewers cannot create folders"; return false; }
        if(!isValidName(name)){ errOut="Invalid name"; return false; }
        // Check for duplicate among direct dir-children of currentDir
        AVLTreeNode* child=currentDir->dirFirstChild;
        while(child){ if(child->name==name){ errOut="Already exists"; return false; } child=child->dirNextSibling; }
        FileMetadata* ex; if(currentDir->files.get(name,ex)){ errOut="File with same name exists"; return false; }
        AVLTreeNode* nn=new AVLTreeNode(name,true,NULL);
        currentDir->addDirChild(nn); // register in dir hierarchy
        root=root->insert(nn);
        root->parent=NULL;
        saveAllFiles();
        return true;
    }

    bool createFileAPI(const string& name,const string& type,const string& content,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        if(!currentUser->hasPermission(PERM_WRITE)){ errOut="Viewers cannot create files"; return false; }
        if(!isValidName(name)){ errOut="Invalid file name"; return false; }
        FileMetadata* ex; if(currentDir->files.get(name,ex)){ errOut="File already exists"; return false; }
        FileMetadata* fm=new FileMetadata(name,type,currentUser->username);
        if(!content.empty()) fm->addVersion(content);
        currentDir->files.insert(name,fm);
        updateRecentFiles(name);
        cloudSync.addSyncTask(getCurrentPath()+"/"+name,"upload");
        saveAllFiles();
        return true;
    }

    bool readFileAPI(const string& name,string& content,bool& compressed,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        FileMetadata* fm; if(!currentDir->files.get(name,fm)){ errOut="File not found"; return false; }
        if(!checkAccess(currentUser,fm,PERM_READ)){ errOut="No read permission"; return false; }
        fm->lastAccessed=time(NULL); fm->accessCount++;
        updateRecentFiles(name);
        if(fm->versions){ content=fm->readDecryptedFromDisk(fm->versions); compressed=fm->versions->compressed; }
        else { content=""; compressed=false; }
        return true;
    }

    bool updateFileAPI(const string& name,const string& content,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        FileMetadata* fm; if(!currentDir->files.get(name,fm)){ errOut="File not found"; return false; }
        if(!checkAccess(currentUser,fm,PERM_WRITE)){ errOut="No write permission"; return false; }
        fm->addVersion(content); updateRecentFiles(name);
        cloudSync.addSyncTask(getCurrentPath()+"/"+name,"upload");
        saveAllFiles();
        return true;
    }

    bool updateSharedFileAPI(const string& receiverUn, const string& senderUn, const string& filename, const string& dirpath, const string& content, string& errOut){
        // Only admins can edit shared files
        User* receiver; if(!users.get(receiverUn,receiver)){ errOut="Receiver not found"; return false; }
        if(!receiver->hasPermission(PERM_MODIFY_ACCESS)){ errOut="Only admins can edit shared files"; return false; }
        // Verify the share exists
        bool found=false;
        vector<SharedFileInfo> shares; getSharedFilesAPI(receiverUn,shares);
        for(auto& s:shares){ if(s.sender==senderUn&&s.filename==filename){found=true;break;} }
        if(!found){ errOut="File not shared with you"; return false; }
        // Find the file in sender's directory
        AVLTreeNode* senderHome=getOrCreateHomeDir(senderUn);
        AVLTreeNode* targetDir=senderHome;
        if(dirpath!="/"){
            size_t start=1;
            while(start<dirpath.size()){
                size_t end=dirpath.find('/',start);
                string seg=(end==string::npos)?dirpath.substr(start):dirpath.substr(start,end-start);
                if(!seg.empty()){
                    AVLTreeNode* next=NULL;
                    Stack<AVLTreeNode*> stk; AVLTreeNode* cur=root;
                    while(cur||!stk.isEmpty()){
                        while(cur){stk.push(cur);cur=cur->left;}
                        cur=stk.pop();
                        if(cur->parent==targetDir&&cur->name==seg&&cur->isDirectory){next=cur;break;}
                        cur=cur->right;
                    }
                    if(!next){ errOut="Sender directory not found"; return false; }
                    targetDir=next;
                }
                start=(end==string::npos)?dirpath.size():end+1;
            }
        }
        FileMetadata* fm=NULL;
        if(!targetDir->files.get(filename,fm)){ errOut="File not found in sender's vault"; return false; }
        // Write new version — this updates the same FileMetadata object sender uses
        fm->addVersion(content);
        saveAllFiles();
        return true;
    }

    bool deleteFileAPI(const string& name,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        FileMetadata* fm; if(!currentDir->files.get(name,fm)){ errOut="File not found"; return false; }
        if(!checkAccess(currentUser,fm,PERM_WRITE)){ errOut="No permission"; return false; }
        // Get or create per-user recycle bin
        Stack<pair<FileMetadata*,string>>* rb=NULL;
        if(!userRecycleBin.get(currentUser->username,rb)){
            rb=new Stack<pair<FileMetadata*,string>>(); userRecycleBin.insert(currentUser->username,rb);
        }
        AVLTreeNode* homeDir=getOrCreateHomeDir(currentUser->username);
        rb->push(make_pair(fm,getCurrentPath(homeDir)));
        currentDir->files.remove(name);
        cloudSync.addSyncTask(getCurrentPath(homeDir)+"/"+name,"delete");
        saveAllFiles();
        return true;
    }

    bool restoreFileAPI(string& restoredName,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        if(!currentUser->hasPermission(PERM_WRITE)){ errOut="Viewers cannot restore files"; return false; }
        Stack<pair<FileMetadata*,string>>* rb=NULL;
        if(!userRecycleBin.get(currentUser->username,rb)||rb->isEmpty()){ errOut="Recycle bin is empty"; return false; }
        auto entry=rb->pop();
        FileMetadata* fm=entry.first; string origPath=entry.second;
        // Navigate to original directory using dir hierarchy
        AVLTreeNode* homeDir=getOrCreateHomeDir(currentUser->username);
        AVLTreeNode* dir=homeDir;
        if(origPath!="/"){
            size_t start=1;
            while(start<origPath.size()){
                size_t end=origPath.find('/',start);
                string seg=(end==string::npos)?origPath.substr(start):origPath.substr(start,end-start);
                if(!seg.empty()){
                    AVLTreeNode* found=NULL;
                    AVLTreeNode* c=dir->dirFirstChild;
                    while(c){ if(c->name==seg&&c->isDirectory){found=c;break;} c=c->dirNextSibling; }
                    if(!found){ dir=homeDir; break; } // fallback to home if path gone
                    dir=found;
                }
                start=(end==string::npos)?origPath.size():end+1;
            }
        }
        FileMetadata* ex; if(dir->files.get(fm->name,ex)){ rb->push(entry); errOut="File already exists at target"; return false; }
        dir->files.insert(fm->name,fm); updateRecentFiles(fm->name);
        restoredName=fm->name;
        saveAllFiles();
        return true;
    }

    bool saveSharedFileAPI(const string& receiverUn, const string& senderUn, const string& filename, const string& dirpath, string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        // Verify share exists and read content
        string content;
        if(!readSharedFileAPI(receiverUn,senderUn,filename,content,errOut)) return false;
        // Check if file already exists in receiver's currentDir
        FileMetadata* ex;
        string saveName=filename;
        // If name conflicts, prefix with sender's name
        if(currentDir->files.get(saveName,ex))
            saveName=senderUn.substr(0,senderUn.find('@'))+"_"+filename;
        // Still conflicts — add number
        int n=1;
        while(currentDir->files.get(saveName,ex))
            saveName=senderUn.substr(0,senderUn.find('@'))+"_"+filename+"_"+to_string(n++);
        // Create new FileMetadata with receiver as owner
        FileMetadata* fm=new FileMetadata(saveName,filename.find('.')!=string::npos?filename.substr(filename.rfind('.')+1):"txt",receiverUn);
        if(!content.empty()) fm->addVersion(content);
        currentDir->files.insert(saveName,fm);
        updateRecentFiles(saveName);
        saveAllFiles();
        return true;
    }

    bool compressFileAPI(const string& name,const string& method,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        FileMetadata* fm; if(!currentDir->files.get(name,fm)){ errOut="File not found"; return false; }
        if(!checkAccess(currentUser,fm,PERM_WRITE)){ errOut="No permission"; return false; }
        if(method!="RLE"&&method!="DICT"){ errOut="Use RLE or DICT"; return false; }
        fm->compress(method);
        cloudSync.addSyncTask(getCurrentPath()+"/"+name,"upload");
        saveAllFiles();
        return true;
    }

    bool decompressFileAPI(const string& name,string& errOut){
        if(!currentUser){ errOut="Not logged in"; return false; }
        FileMetadata* fm; if(!currentDir->files.get(name,fm)){ errOut="File not found"; return false; }
        if(!checkAccess(currentUser,fm,PERM_WRITE)){ errOut="No permission"; return false; }
        fm->decompress();
        saveAllFiles();
        return true;
    }

    bool addConnectionAPI(const string& callerUn,const string& targetUn,string& errOut){
        if(callerUn==targetUn){ errOut="Cannot connect to yourself"; return false; }
        User* t; if(!users.get(targetUn,t)){ errOut="User not found"; return false; }
        userGraph.addConnection(callerUn,targetUn);
        saveConnections();
        return true;
    }

    bool shareFileAPI(const string& callerUn,const string& filename,const string& receiver,string& errOut){
        if(callerUn==receiver){ errOut="Cannot share with yourself"; return false; }
        User* caller; if(!users.get(callerUn,caller)){ errOut="User not found"; return false; }
        if(!caller->hasPermission(PERM_WRITE)){ errOut="Viewers cannot share files"; return false; }
        FileMetadata* fm; if(!currentDir->files.get(filename,fm)){ errOut="File not found"; return false; }
        User* rv; if(!users.get(receiver,rv)){ errOut="Receiver not found"; return false; }
        if(!userGraph.isConnected(callerUn,receiver)){ errOut="Connect with user first before sharing"; return false; }
        // Store as "filename:dirpath" so receiver can locate exact directory
        AVLTreeNode* homeDir=getOrCreateHomeDir(callerUn);
        string dirpath=getCurrentPath(homeDir);
        string record=filename+":"+dirpath;
        if(!userGraph.shareFile(callerUn,receiver,record)){ errOut="Share failed"; return false; }
        saveConnections();
        return true;
    }

    struct SharedFileInfo { string sender, filename, dirpath; };
    void getSharedFilesAPI(const string& un, vector<SharedFileInfo>& out){
        Queue<string> q; userGraph.getSharedFiles(un,q);
        while(!q.isEmpty()){
            string item=q.dequeue();
            // item format from UserGraph: "sender|filename:dirpath"
            size_t pipe=item.find('|');
            if(pipe==string::npos) continue;
            string sender=item.substr(0,pipe);
            string rest=item.substr(pipe+1);
            // rest is "filename:dirpath"
            size_t colon=rest.find(':');
            string fname=(colon==string::npos)?rest:rest.substr(0,colon);
            string dpath=(colon==string::npos)?"/":rest.substr(colon+1);
            out.push_back({sender, fname, dpath});
        }
    }

    bool readSharedFileAPI(const string& receiverUn, const string& senderUn, const string& filename, string& content, string& errOut){
        if(!userGraph.isConnected(receiverUn, senderUn)){ errOut="Not connected with sender"; return false; }
        SharedFileInfo found_share; bool found=false;
        vector<SharedFileInfo> shares; getSharedFilesAPI(receiverUn, shares);
        for(auto& s:shares){ if(s.sender==senderUn&&s.filename==filename){found_share=s;found=true;break;} }
        if(!found){ errOut="File not shared with you"; return false; }
        AVLTreeNode* senderHome=getOrCreateHomeDir(senderUn);
        AVLTreeNode* targetDir=senderHome;
        string dpath=found_share.dirpath;
        if(dpath!="/"){
            size_t start=1;
            while(start<dpath.size()){
                size_t end=dpath.find('/',start);
                string seg=(end==string::npos)?dpath.substr(start):dpath.substr(start,end-start);
                if(!seg.empty()){
                    AVLTreeNode* next=NULL;
                    AVLTreeNode* c=targetDir->dirFirstChild;
                    while(c){ if(c->name==seg&&c->isDirectory){next=c;break;} c=c->dirNextSibling; }
                    if(!next){ errOut="Sender directory not found"; return false; }
                    targetDir=next;
                }
                start=(end==string::npos)?dpath.size():end+1;
            }
        }
        FileMetadata* fm=NULL;
        if(!targetDir->files.get(filename,fm)){ errOut="File not found in sender's vault"; return false; }
        if(!fm->versions||fm->versions->diskPath.empty()){ errOut="File has no content"; return false; }
        content=fm->readDecryptedFromDisk(fm->versions);
        return true;
    }

    void getConnectionsAPI(const string& un,vector<string>& out){
        Queue<string> q; userGraph.getConnections(un,q);
        while(!q.isEmpty()) out.push_back(q.dequeue());
    }

    void getRecentFilesAPI(vector<string>& out){
        if(!currentUser) return;
        Queue<string>* rq=NULL;
        if(!userRecentFiles.get(currentUser->username,rq)) return;
        Queue<string> tmp=*rq;
        Stack<string> stk; while(!tmp.isEmpty()) stk.push(tmp.dequeue());
        while(!stk.isEmpty()) out.push_back(stk.pop());
    }

    bool isSyncOnline(){ return cloudSync.isOnline(); }
    bool isAdmin(const string& un){ User* u; if(!users.get(un,u)) return false; return u->hasPermission(PERM_MODIFY_ACCESS); }

    // Expose users list for admin panel
    struct UserPublicInfo { string username,role; bool isLoggedIn; time_t lastLogin; };
    void getUsersAPI(vector<UserPublicInfo>& out){
        users.iterate([&](const string&,User* u){
            out.push_back({u->username,u->getRole(),u->isLoggedIn,u->lastLogin});
        });
    }

    string getSecurityQuestion(const string& un){
        User* u; if(users.get(un,u)) return u->securityQuestion; return "";
    }
};

// ============================================================
//  JSON Helpers
// ============================================================
static string jEscape(const string& s){
    string o;
    for(char c:s){
        if(c=='"') o+="\\\"";
        else if(c=='\\') o+="\\\\";
        else if(c=='\n') o+="\\n";
        else if(c=='\r') o+="\\r";
        else if(c=='\t') o+="\\t";
        else o+=c;
    }
    return o;
}
static string jOk(const string& extra=""){
    return "{\"success\":true"+(extra.empty()?"":","+extra)+"}";
}
static string jErr(const string& msg){
    return "{\"success\":false,\"error\":\""+jEscape(msg)+"\"}";
}
static string jStr(const string& k,const string& v){
    return "\""+k+"\":\""+jEscape(v)+"\"";
}
static string jBool(const string& k,bool v){
    return "\""+k+"\":"+string(v?"true":"false");
}

// Simple JSON field parser
static string jGet(const string& body,const string& key){
    string search="\""+key+"\"";
    size_t pos=body.find(search);
    if(pos==string::npos) return "";
    pos+=search.size();
    while(pos<body.size()&&(body[pos]==' '||body[pos]==':')) pos++;
    if(pos>=body.size()) return "";
    if(body[pos]=='"'){
        pos++;
        string val;
        while(pos<body.size()&&body[pos]!='"'){
            if(body[pos]=='\\'&&pos+1<body.size()){
                pos++;
                if(body[pos]=='n') val+='\n';
                else if(body[pos]=='t') val+='\t';
                else if(body[pos]=='r') val+='\r';
                else val+=body[pos];
            } else val+=body[pos];
            pos++;
        }
        return val;
    }
    string val;
    while(pos<body.size()&&body[pos]!=','&&body[pos]!='}') val+=body[pos++];
    size_t e=val.find_last_not_of(" \t\n\r");
    return (e==string::npos)?"":(val.substr(0,e+1));
}

// ============================================================
//  main() — HTTP Server
// ============================================================
int main(){
    FileSystem fs;
    using namespace httplib;
    Server svr;

    // CORS
    auto cors=[](Response& res){
        res.set_header("Access-Control-Allow-Origin","*");
        res.set_header("Access-Control-Allow-Headers","Content-Type");
        res.set_header("Access-Control-Allow-Methods","POST,OPTIONS");
    };

    svr.Options(".*",[&](const Request&,Response& res){ cors(res); res.set_content("","text/plain"); });

    // ---- /signup ----
    svr.Post("/signup",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),pw=jGet(req.body,"password"),
               q=jGet(req.body,"question"),a=jGet(req.body,"answer"),role=jGet(req.body,"role");
        string err;
        if(fs.signupAPI(un,pw,q,a,role,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /login  (step 1: validate creds, emit OTP to file) ----
    svr.Post("/login",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),pw=jGet(req.body,"password"),err;
        if(fs.loginStep1(un,pw,err))
            res.set_content(jOk("\"message\":\"OTP written to OTP.txt\""),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /verify_otp  (step 2: verify OTP, complete login) ----
    svr.Post("/verify_otp",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),otp=jGet(req.body,"otp"),err;
        if(fs.loginStep2(un,otp,err)){
            fs.setCurrentUser(un);
            // get role
            vector<FileSystem::UserPublicInfo> users;
            fs.getUsersAPI(users);
            string role="viewer";
            for(auto& u:users) if(u.username==un){role=u.role;break;}
            res.set_content(jOk(jStr("role",role)),"application/json");
        } else {
            res.set_content(jErr(err),"application/json");
        }
    });

    // ---- /logout ----
    svr.Post("/logout",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),err;
        if(fs.logoutAPI(un,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /reset_password ----
    svr.Post("/reset_password",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),ans=jGet(req.body,"answer"),np=jGet(req.body,"new_password"),err;
        if(fs.resetPasswordAPI(un,ans,np,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /security_question ----
    svr.Post("/security_question",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username");
        string q=fs.getSecurityQuestion(un);
        if(q.empty()) res.set_content(jErr("User not found"),"application/json");
        else res.set_content(jOk(jStr("question",q)),"application/json");
    });

    // ---- /set_role ----
    svr.Post("/set_role",[&](const Request& req,Response& res){
        cors(res);
        string caller=jGet(req.body,"username"),target=jGet(req.body,"target_username"),role=jGet(req.body,"role"),err;
        fs.setCurrentUser(caller);
        if(fs.setUserRoleAPI(caller,target,role,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /cd ----
    svr.Post("/cd",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),path=jGet(req.body,"path"),err,newPath;
        fs.setCurrentUser(un);
        if(fs.cdAPI(path,err,newPath))
            res.set_content(jOk(jStr("current_path",newPath)),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /list ----
    svr.Post("/list",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username");
        fs.setCurrentUser(un);
        vector<FileSystem::FileInfo> files; vector<FileSystem::DirInfo> dirs;
        fs.listAPI(files,dirs);
        // Build JSON arrays
        string fa="[",da="["; bool ff=true,fd=true;
        for(auto& f:files){
            if(!ff) fa+=","; ff=false;
            fa+="{"+jStr("name",f.name)+","+jStr("type",f.type)+","+jStr("owner",f.owner)+","+jBool("compressed",f.compressed)+"}";
        }
        for(auto& d:dirs){
            if(!fd) da+=","; fd=false;
            da+="{"+jStr("name",d.name)+"}";
        }
        fa+="]"; da+="]";
        res.set_content(jOk("\"files\":"+fa+",\"dirs\":"+da),"application/json");
    });

    // ---- /mkdir ----
    svr.Post("/mkdir",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"dirname"),err;
        fs.setCurrentUser(un);
        if(fs.mkdirAPI(name,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /create_file ----
    svr.Post("/create_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"filename"),
               type=jGet(req.body,"type"),content=jGet(req.body,"content"),err;
        fs.setCurrentUser(un);
        if(fs.createFileAPI(name,type,content,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /read_file ----
    svr.Post("/read_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"filename"),content,err;
        bool compressed=false;
        fs.setCurrentUser(un);
        if(fs.readFileAPI(name,content,compressed,err))
            res.set_content(jOk(jStr("content",content)+","+jBool("compressed",compressed)),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /update_file ----
    svr.Post("/update_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"filename"),content=jGet(req.body,"content"),err;
        fs.setCurrentUser(un);
        if(fs.updateFileAPI(name,content,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /delete_file ----
    svr.Post("/delete_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"filename"),err;
        fs.setCurrentUser(un);
        if(fs.deleteFileAPI(name,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /save_shared_file ----
    svr.Post("/save_shared_file",[&](const Request& req,Response& res){
        cors(res);
        string receiver=jGet(req.body,"username");
        string sender  =jGet(req.body,"sender");
        string filename=jGet(req.body,"filename");
        string dirpath =jGet(req.body,"dirpath");
        fs.setCurrentUser(receiver);
        string err;
        if(fs.saveSharedFileAPI(receiver,sender,filename,dirpath,err))
            res.set_content(jOk(),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /restore_file ----
    svr.Post("/restore_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),restored,err;
        fs.setCurrentUser(un);
        if(fs.restoreFileAPI(restored,err))
            res.set_content(jOk(jStr("restored",restored)),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /compress_file ----
    svr.Post("/compress_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"filename"),method=jGet(req.body,"method"),err;
        if(method.empty()) method="RLE";
        fs.setCurrentUser(un);
        if(fs.compressFileAPI(name,method,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /decompress_file ----
    svr.Post("/decompress_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),name=jGet(req.body,"filename"),err;
        fs.setCurrentUser(un);
        if(fs.decompressFileAPI(name,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /add_connection ----
    svr.Post("/add_connection",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),target=jGet(req.body,"target"),err;
        if(fs.addConnectionAPI(un,target,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /share_file ----
    svr.Post("/share_file",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username"),fname=jGet(req.body,"filename"),receiver=jGet(req.body,"receiver"),err;
        fs.setCurrentUser(un);
        if(fs.shareFileAPI(un,fname,receiver,err)) res.set_content(jOk(),"application/json");
        else res.set_content(jErr(err),"application/json");
    });

    // ---- /shared ----
    svr.Post("/shared",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username");
        vector<FileSystem::SharedFileInfo> items; fs.getSharedFilesAPI(un,items);
        string arr="["; bool first=true;
        for(auto& s:items){
            if(!first) arr+=",";
            arr+="{"+jStr("sender",s.sender)+","+jStr("filename",s.filename)+"}";
            first=false;
        }
        arr+="]";
        res.set_content(jOk("\"files\":"+arr),"application/json");
    });

    // ---- /read_shared_file ----
    svr.Post("/read_shared_file",[&](const Request& req,Response& res){
        cors(res);
        string receiver=jGet(req.body,"username");
        string sender  =jGet(req.body,"sender");
        string filename=jGet(req.body,"filename");
        fs.setCurrentUser(receiver);
        string content,err;
        // Get dirpath from share record
        string dirpath="/";
        vector<FileSystem::SharedFileInfo> shares; fs.getSharedFilesAPI(receiver,shares);
        for(auto& s:shares){ if(s.sender==sender&&s.filename==filename){dirpath=s.dirpath;break;} }
        bool isAdmin=fs.isAdmin(receiver);
        if(fs.readSharedFileAPI(receiver,sender,filename,content,err))
            res.set_content(jOk(
                jStr("content",content)+","+
                jStr("filename",filename)+","+
                jStr("sender",sender)+","+
                jStr("dirpath",dirpath)+","+
                jBool("is_admin",isAdmin)
            ),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /update_shared_file ----
    svr.Post("/update_shared_file",[&](const Request& req,Response& res){
        cors(res);
        string receiver=jGet(req.body,"username");
        string sender  =jGet(req.body,"sender");
        string filename=jGet(req.body,"filename");
        string dirpath =jGet(req.body,"dirpath");
        string content =jGet(req.body,"content");
        fs.setCurrentUser(receiver);
        string err;
        if(fs.updateSharedFileAPI(receiver,sender,filename,dirpath,content,err))
            res.set_content(jOk(),"application/json");
        else
            res.set_content(jErr(err),"application/json");
    });

    // ---- /connections ----
    svr.Post("/connections",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username");
        vector<string> items; fs.getConnectionsAPI(un,items);
        string arr="["; bool first=true;
        for(auto& c:items){ if(!first) arr+=","; arr+="{"+jStr("username",c)+"}"; first=false; }
        arr+="]";
        res.set_content(jOk("\"connections\":"+arr),"application/json");
    });

    // ---- /recent ----
    svr.Post("/recent",[&](const Request& req,Response& res){
        cors(res);
        string un=jGet(req.body,"username");
        fs.setCurrentUser(un);
        vector<string> items; fs.getRecentFilesAPI(items);
        string arr="["; bool first=true;
        for(auto& f:items){ if(!first) arr+=","; arr+="{"+jStr("name",f)+"}"; first=false; }
        arr+="]";
        res.set_content(jOk("\"files\":"+arr),"application/json");
    });

    // ---- /sync_status ----
    svr.Post("/sync_status",[&](const Request& req,Response& res){
        cors(res);
        bool online=fs.isSyncOnline();
        res.set_content(jOk(jBool("online",online)),"application/json");
    });

    // ---- /users ----
    svr.Post("/users",[&](const Request& req,Response& res){
        cors(res);
        vector<FileSystem::UserPublicInfo> users;
        fs.getUsersAPI(users);
        string arr="["; bool first=true;
        for(auto& u:users){
            if(!first) arr+=",";
            arr+="{"+jStr("username",u.username)+","+jStr("role",u.role)+","+jBool("isLoggedIn",u.isLoggedIn)+",\"lastLogin\":"+to_string(u.lastLogin)+"}";
            first=false;
        }
        arr+="]";
        res.set_content(jOk("\"users\":"+arr),"application/json");
    });

    // ---- Server ready ----
    cout << CYAN << "\n";
    cout << "  ________________________________________\n";
    cout << "  |   DataVault Server  ·  Port 8080     |\n";
    cout << "  |   Open datavault.html in browser     |\n";
    cout << "  |   Press Ctrl+C to stop               |\n";
    cout << "  ________________________________________\n";
    cout << RESET << "\n";

    svr.listen("0.0.0.0", 8080);
    return 0;
}
