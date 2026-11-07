/*
 * 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程
 * 应用层示例
 * https://github.com/SDGOODS/SDGOODS-ESP32S3
 *
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * 「谷仓共创计划」与「谷仓 SDGOODS 开放平台」项目、谷仓次元屏（谷仓电子徽章）设备，
 *   以及本基础代码的著作权与相关权利，均归深圳希德创新网络有限公司所有。
 * SPDX-License-Identifier: Apache-2.0
 *
 * 本文件属于应用层，以 Apache-2.0 发布：可自由商用、可闭源分发，
 * 只需保留本声明并携带 NOTICE 文件。详见 LICENSING.md。
 */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Source for Hangzhou FM identities: docs/RADIO_STATION_IDENTITY.md.
 */
#include "radio_station_identity.h"
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

typedef struct { const char *scope, *alias, *title; float mhz; } station_identity_t;
static const station_identity_t s_identities[]={
    {NULL,"西湖之声","西湖之声",105.4f},
    {"杭州","交通经济广播","交通广播",91.8f},
    {"杭州","交通91.8","交通广播",91.8f},
    {"杭州","交通广播","交通广播",91.8f},
    {"杭州","城市资讯","城市资讯",90.7f},
    {NULL,"杭州之声","杭州之声",89.0f},
    {NULL,"华语之声","华语之声",0.0f},
    {NULL,"中国之声","中国之声",0.0f},
    {NULL,"经济之声","经济之声",0.0f},
    {NULL,"音乐之声","音乐之声",0.0f},
};

static const station_identity_t *identity(const char *name)
{
    if(!name)return NULL;
    for(size_t i=0;i<sizeof(s_identities)/sizeof(s_identities[0]);++i){
        const station_identity_t *entry=&s_identities[i];
        if(strstr(name,entry->alias) && (!entry->scope || strstr(name,entry->scope)))return entry;
    }
    return NULL;
}

float radio_station_known_frequency(const char *name)
{
    const station_identity_t *entry=identity(name);
    return entry?entry->mhz:0.0f;
}

void radio_station_copy_utf8(const char *name, char *out, size_t capacity)
{
    if(!out || !capacity)return;
    if(!name)name="";
    size_t used=0;
    while(*name){
        unsigned char lead=(unsigned char)*name;
        size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:4;
        bool complete=true;
        for(size_t i=1;i<length;++i)if(!name[i] || ((unsigned char)name[i]&0xc0)!=0x80){complete=false;break;}
        if(!complete || used+length>=capacity)break;
        memcpy(out+used,name,length);used+=length;name+=length;
    }
    out[used]=0;
}

void radio_station_short_name(const char *name, char *out, size_t capacity)
{
    if(!out || !capacity)return;
    const station_identity_t *entry=identity(name);
    if(entry){radio_station_copy_utf8(entry->title,out,capacity);return;}
    if(!name)name="";
    const char *start=name;
    /* Drop the owner only when there is a nonempty station brand after it. */
    const char *owners[]={"广播电视总台","广播电视台","人民广播电台","广播电台"};
    for(size_t i=0;i<sizeof(owners)/sizeof(owners[0]);++i){
        const char *hit=strstr(start,owners[i]);
        if(hit && hit[strlen(owners[i])]){start=hit+strlen(owners[i]);break;}
    }
    if((start[0]=='F'||start[0]=='f') && (start[1]=='M'||start[1]=='m') && isdigit((unsigned char)start[2])){
        const char *p=start+2;
        while(isdigit((unsigned char)*p)||*p=='.'||*p==' ')++p;
        if(*p)start=p;
    }
    while(*start==' '||*start=='-'||*start==':')++start;
    radio_station_copy_utf8(start,out,capacity);
    const char *suffixes[]={"网络电台","网络广播"};
    for(size_t i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);++i){
        size_t length=strlen(out),tail=strlen(suffixes[i]);
        if(length>=tail+12 && !strcmp(out+length-tail,suffixes[i])){out[length-tail]=0;break;}
    }
}
