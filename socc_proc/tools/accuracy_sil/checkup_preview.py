"""PC软件渲染生产LVGL引导卡；保存像素与检查按钮/标签边界，不验证板级GPU。"""
from pathlib import Path
import subprocess
import hashlib
from cross_layer_sil import ROOT, VCVARS

OUT=ROOT / 'cache/checkup-preview'
UI=ROOT / 'Edgi_Talk_M55_LVGL/applications/ui'
LV=ROOT / 'Edgi_Talk_M55_LVGL/libraries/components/lvgl_9.2.0'


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    config='''#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 32
#define LV_USE_OS 0
#define LV_USE_STDLIB_MALLOC 1
#define LV_USE_STDLIB_STRING 1
#define LV_USE_STDLIB_SPRINTF 1
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(ui_font_H1) LV_FONT_DECLARE(ui_font_Title) LV_FONT_DECLARE(ui_font_Subtitle) LV_FONT_DECLARE(ui_font_Number_big) LV_FONT_DECLARE(ui_font_Number_extra)
#endif
'''
    (OUT/'lv_conf.h').write_text(config,encoding='utf-8')
    (OUT/'rtthread.h').write_text('''#pragma once
#include <stddef.h>
#include <stdio.h>
typedef void *rt_mutex_t;
typedef int rt_bool_t;
#define RT_NULL NULL
#define RT_IPC_FLAG_PRIO 1
#define RT_WAITING_FOREVER -1
#define rt_snprintf snprintf
static inline rt_mutex_t rt_mutex_create(const char *s,int f) {(void)s;(void)f;return (void *)1;}
static inline int rt_mutex_take(rt_mutex_t m,int t) {(void)m;(void)t;return 0;}
static inline int rt_mutex_release(rt_mutex_t m) {(void)m;return 0;}
''',encoding='utf-8')
    c=r'''
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "checkup_widgets.h"
#include "src/misc/lv_text_private.h"
#include "ui_tab_monitor.h"
#include "ui_tab_report.h"
static checkup_flow_t flow;
static struct ui_health_data data;
static uint32_t frame[512*800];
static uint32_t buffer[512*800];
extern const uint8_t noto_gb2312_ttf[];
extern const uint32_t noto_gb2312_ttf_size;
lv_font_t *ui_cn_font_get(int px) {
    static lv_font_t *font;
    if(!font)font=lv_tiny_ttf_create_data_ex(noto_gb2312_ttf,noto_gb2312_ttf_size,px,LV_FONT_KERNING_NONE,96);
    return font;
}
const checkup_flow_t *ui_checkup_get(void) {return &flow;}
const struct ui_health_data *ui_data_get(void) {return &data;}
void ui_checkup_start(void) {checkup_start(&flow,lv_tick_get(),&data);}
void ui_checkup_retry(unsigned item) {checkup_select(&flow,item,lv_tick_get(),&data);}
void ui_checkup_skip(void) {checkup_skip(&flow,lv_tick_get(),&data);}
void ui_checkup_cancel(void) {checkup_cancel(&flow,lv_tick_get());}
void ui_show_report(void) {}
void ui_checkup_report(struct ui_health_data *out) {*out=flow.report;}
int advice_local_generate(char *b,int n) {(void)b;(void)n;return 0;}
int mqtt_advice_ready(void) {return 0;}
void mqtt_advice_request(void) {}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *pixels) {
    uint32_t *p=(uint32_t *)pixels;
    for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++)frame[y*512+x]=*p++;
    lv_display_flush_ready(d);
}
static void save(const char *name) {
    FILE *f=fopen(name,"wb");assert(f);fprintf(f,"P6\n512 800\n255\n");
    for(int i=0;i<512*800;i++) {
        unsigned char rgb[3]={(frame[i]>>16)&255,(frame[i]>>8)&255,frame[i]&255};
        fwrite(rgb,1,3,f);
    }
    fclose(f);
}
static void glyphs(const char *txt) {
    uint32_t at=0,letter;
    while((letter=lv_text_encoded_next(txt,&at))) {
        if(letter<32)continue;
        lv_font_glyph_dsc_t dsc;
        assert(lv_font_get_glyph_dsc(ui_cn_font_get(18),&dsc,letter,0) && !dsc.is_placeholder);
    }
}
int main(void) {
    lv_init();
    lv_display_t *display=lv_display_create(512,800);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *scr=lv_screen_active();lv_obj_set_style_bg_color(scr,UI_COLOR_BG,0);
    lv_obj_t *page=lv_obj_create(scr);lv_obj_remove_style_all(page);
    lv_obj_set_pos(page,51,8);lv_obj_set_size(page,UI_CARD_FULL_W,720);
    lv_obj_set_flex_flow(page,LV_FLEX_FLOW_COLUMN);
    checkup_widgets_t w={0};checkup_widgets_create(&w,page,1);
    const int states[]={CHECKUP_IDLE,CHECKUP_PPG,CHECKUP_PPG_FAILED,CHECKUP_ECG,CHECKUP_ECG_FAILED,CHECKUP_DONE,CHECKUP_CANCELLED};
    for(int i=0;i<7;i++) {
        flow.state=states[i];flow.id=i?1:0;flow.completed=i>=3?1:0;flow.resolved=i==5?3:flow.completed;
        flow.reason=UI_REJ_SPO2;flow.ecg_request=1;data.sensor_link_valid=1;data.ecg_session_id=1;
        checkup_widgets_refresh(&w);lv_obj_update_layout(scr);
        lv_tick_inc(40);lv_refr_now(display);
        lv_area_t card;lv_obj_get_coords(w.card,&card);
        assert(card.x1>=51 && card.x2<480 && card.y2<720);
        lv_obj_t *labels[]={w.title,w.hint,w.items,w.note};
        for(int j=0;j<4;j++) {
            lv_area_t a;lv_obj_get_coords(labels[j],&a);
            assert(a.x1>=card.x1 && a.x2<=card.x2 && a.y2<=card.y2);
            glyphs(lv_label_get_text(labels[j]));
        }
        for(int j=0;j<4;j++) {
            lv_area_t a,b;lv_obj_get_coords(w.buttons[j],&a);lv_obj_get_coords(w.labels[j],&b);
            assert(b.x1>=a.x1 && b.x2<=a.x2 && b.y1>=a.y1 && b.y2<=a.y2);
            glyphs(lv_label_get_text(w.labels[j]));
        }
        char filename[40];sprintf(filename,"checkup-%d.ppm",i);save(filename);
    }
    flow.state=CHECKUP_IDLE;
    for(int reason=0;reason<=UI_REJ_REQUEST;reason++)glyphs(checkup_reason(reason));
    lv_obj_send_event(w.buttons[0],LV_EVENT_CLICKED,NULL);assert(flow.state==CHECKUP_PPG);
    lv_obj_send_event(w.buttons[1],LV_EVENT_CLICKED,NULL);assert(flow.state==CHECKUP_ECG);
    lv_obj_send_event(w.buttons[2],LV_EVENT_CLICKED,NULL);assert(flow.state==CHECKUP_CANCELLED);
    puts("LVGL: 7 states rendered; card/label/button bounds and button events passed");
    /* 实际监测页和报告页布局；只替换数据源/联网/RTOS，保留生产页面代码。 */
    lv_obj_add_flag(page,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *monitor=lv_obj_create(scr);lv_obj_remove_style_all(monitor);
    lv_obj_set_size(monitor,512,740);
    lv_obj_set_style_pad_left(monitor,51,0);lv_obj_set_style_pad_right(monitor,16,0);
    flow.state=CHECKUP_PPG;flow.id=1;flow.completed=flow.resolved=0;flow.started_ms=lv_tick_get();
    data.ppg_session_state=UI_MEAS_WAIT_FINGER;
    ui_tab_monitor_create(monitor);
    lv_tick_inc(1100);lv_timer_handler();lv_obj_update_layout(scr);lv_refr_now(display);save("monitor.ppm");
    assert(lv_obj_get_scroll_bottom(monitor)>0);
    lv_obj_add_flag(monitor,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *report=lv_obj_create(scr);lv_obj_remove_style_all(report);
    lv_obj_set_size(report,512,740);
    lv_obj_set_style_pad_left(report,51,0);lv_obj_set_style_pad_right(report,16,0);
    flow.state=CHECKUP_DONE;flow.completed=1;flow.resolved=3;flow.finished_ms=lv_tick_get();
    flow.report.heart_rate=76;flow.report.hr_valid=1;flow.report.spo2=98;flow.report.spo2_valid=1;
    ui_tab_report_create(report);
    lv_tick_inc(1100);lv_timer_handler();lv_obj_update_layout(scr);lv_refr_now(display);save("report.ppm");
    puts("LVGL: production monitor/report pages rendered with fixture data");
    lv_deinit();return 0;
}
'''
    (OUT/'preview.c').write_text(c,encoding='utf-8')
    # 只移除ARM链接段属性；字体二进制保持原样。
    import re
    font_text=(UI/'fonts/noto_gb2312_ttf.c').read_text(encoding='utf-8')
    font_text=re.sub(r'^__attribute__[^\n]*\n', '', font_text, flags=re.M)
    (OUT/'noto_gb2312_ttf.c').write_text(font_text,encoding='utf-8')
    sources=list((LV/'src').rglob('*.c'))+[UI/'ui_common.c',UI/'ui_tab_monitor.c',UI/'ui_tab_report.c',UI.parent/'health_report.c']+list((UI/'fonts').glob('ui_font_*.c'))+[OUT/'noto_gb2312_ttf.c',OUT/'preview.c']
    # 拷贝未修改的板级刷新实现，避免同目录固件lv_conf.h遮蔽PC配置。
    import shutil
    shutil.copy2(ROOT/'Edgi_Talk_M55_LVGL/libraries/Common/board/ports/lvgl/lv_refr.c',OUT/'lv_refr.c')
    sources.append(OUT/'lv_refr.c')
    names=[p.stem for p in sources]
    assert len(set(names))==len(names),'object basename collision'
    flags=f'/nologo /utf-8 /O1 /std:c11 /D_CRT_SECURE_NO_WARNINGS /DLV_CONF_INCLUDE_SIMPLE /I"{OUT}" /I"{LV}" /I"{UI}"'
    flags+=' '+' '.join('/I"'+str(p)+'"' for p in sorted({p.parent for p in (LV/'src').rglob('*.h')}))
    objs=OUT/'obj';objs.mkdir(exist_ok=True)
    # 只编译改动输入；配置改变时全编译。日志和中间文件留在cache。
    stamp=hashlib.sha256(config.encode()).hexdigest()
    rebuild=not (OUT/'config.sha256').exists() or (OUT/'config.sha256').read_text()!=stamp
    pending=[p for p in sources if rebuild or not (objs/(p.stem+'.obj')).exists() or p.stat().st_mtime>(objs/(p.stem+'.obj')).stat().st_mtime]
    rsp=OUT/'compile.rsp'
    rsp.write_text(flags+' /MP4 /c /Fo"'+objs.as_posix()+'/"\n'+'\n'.join('"'+str(p)+'"' for p in pending),encoding='utf-8')
    link=OUT/'link.rsp'
    link.write_text('/nologo /OUT:"'+str(OUT/'preview.exe')+'"\n'+'\n'.join('"'+str(objs/(p.stem+'.obj'))+'"' for p in sources),encoding='utf-8')
    batch=OUT/'build.cmd'
    batch.write_text(f'@echo off\ncall "{VCVARS}" >nul 2>&1\n'+
        (f'cl @"{rsp}"\nif errorlevel 1 exit /b 1\n' if pending else '')+
        f'link @"{link}"\n',encoding='utf-8')
    with (OUT/'build.log').open('wb') as log:
        subprocess.run(['cmd','/c',str(batch)],cwd=OUT,stdout=log,stderr=subprocess.STDOUT,check=True)
    (OUT/'config.sha256').write_text(stamp)
    subprocess.run([str(OUT/'preview.exe')],cwd=OUT,check=True)
    print(f'Preview images: {OUT}')


if __name__=='__main__':main()
