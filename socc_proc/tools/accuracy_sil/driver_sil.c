/* Compile the production drivers with a deterministic failing I2C/mux. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "MAX30102.h"
#include "sht30.h"
#include "tca9548a.h"
#include "ads1015.h"
#include "mlx90614.h"
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)
static struct rt_i2c_bus_device bus;
static struct tca9548a_device mux;
static int regs[256], fail_reg=-1, short_fifo, transfers, fifo_reads;
static int lock_fail, locks, releases, mux_held, selections, select_fail, send_fail, recv_fail, crc_bad;
static unsigned char selected;
static int adc_ready_after, adc_polls, adc_conversion_reads, adc_config_fail, adc_read_fail, adc_send_fail;
static int delays, mlx_bad_pec;
rt_mutex_t rt_mutex_create(const char *n,int f) { (void)n;(void)f;return (void *)1; }
rt_err_t rt_mutex_take(rt_mutex_t m,int t) { (void)m;(void)t; if(lock_fail)return -RT_ERROR;locks++;return 0; }
rt_err_t rt_mutex_release(rt_mutex_t m) { (void)m;releases++;return 0; }
rt_err_t rt_mutex_delete(rt_mutex_t m) { (void)m;return 0; }
struct rt_i2c_bus_device *rt_i2c_bus_device_find(const char *n) { (void)n;return &bus; }
tca9548a_device_t tca9548a_shared(void) { return &mux; }
rt_err_t tca9548a_lock(tca9548a_device_t m) { (void)m;CHECK(!mux_held);mux_held=1;return 0; }
void tca9548a_unlock(tca9548a_device_t m) { (void)m;CHECK(mux_held);mux_held=0; }
rt_err_t tca9548a_select_channel(tca9548a_device_t m,rt_uint8_t ch) {
    (void)m;CHECK(mux_held);selections++;if(selections==select_fail)return -RT_ERROR;selected=ch;return 0;
}
void rt_thread_mdelay(int ms) {
    delays+=ms;
    if(ms==20) { CHECK(!mux_held); selected=0; } /* Another task uses CH0 during conversion. */
}
static unsigned char crc(const unsigned char *p) {
    unsigned char c=255;for(int i=0;i<2;i++){c^=p[i];for(int j=0;j<8;j++)c=(c&128)?(c<<1)^0x31:c<<1;}return c;
}
rt_size_t rt_i2c_transfer(struct rt_i2c_bus_device *b,struct rt_i2c_msg *m,rt_uint32_t n) {
    (void)b;CHECK(n==2);transfers++;int reg=m[0].buf[0];
    if(m[0].addr==0x5A) {
        unsigned char bytes[]={0xB4,(unsigned char)reg,0xB5,0x98,0x3A};
        unsigned char pec=0;
        for(int i=0;i<5;i++){pec^=bytes[i];for(int j=0;j<8;j++)pec=(pec&128)?(pec<<1)^7:pec<<1;}
        m[1].buf[0]=bytes[3];m[1].buf[1]=bytes[4];m[1].buf[2]=pec^(mlx_bad_pec?1:0);
        return 2;
    }
    if(m[0].addr==ADS1015_ADDR) {
        if(reg==1) {
            adc_polls++;
            if(adc_config_fail)return 1;
            m[1].buf[0]=(adc_polls>=adc_ready_after)?0x80:0; m[1].buf[1]=0;
        } else {
            adc_conversion_reads++;
            if(adc_read_fail)return 1;
            m[1].buf[0]=0x12;m[1].buf[1]=0x30;
        }
        return 2;
    }
    if(reg==fail_reg)return 1;
    if(reg==REG_FIFO_DATA) {
        fifo_reads++;if(short_fifo)return 1;
        for(int i=0;i<m[1].len;i++)m[1].buf[i]=(unsigned char)(i+1);
    } else for(int i=0;i<m[1].len;i++)m[1].buf[i]=(unsigned char)regs[reg+i];
    return 2;
}
rt_size_t rt_i2c_master_send(struct rt_i2c_bus_device *b,rt_uint16_t a,rt_uint16_t f,const rt_uint8_t *p,rt_uint32_t n) {
    (void)b;(void)f;transfers++;
    if(a==ADS1015_ADDR && adc_send_fail)return 0;
    if(a==0x44) { CHECK(mux_held && selected==3);if(send_fail)return 0; }
    if(a==0x57 && n==2)regs[p[0]]=p[1];return n;
}
rt_size_t rt_i2c_master_recv(struct rt_i2c_bus_device *b,rt_uint16_t a,rt_uint16_t f,rt_uint8_t *p,rt_uint32_t n) {
    (void)b;(void)f;CHECK(a==0x44 && mux_held && selected==3);transfers++;
    if(recv_fail)return 0;
    p[0]=0x66;p[1]=0x66;p[2]=crc(p);p[3]=0x80;p[4]=0;p[5]=crc(p+3);
    if(crc_bad)p[2]^=1;return n;
}
static void reset(void) {
    memset(regs,0,sizeof(regs));fail_reg=-1;short_fifo=transfers=fifo_reads=0;
    lock_fail=locks=releases=mux_held=selections=select_fail=send_fail=recv_fail=crc_bad=0;
    adc_ready_after=1;adc_polls=adc_conversion_reads=adc_config_fail=adc_read_fail=adc_send_fail=0;
    delays=mlx_bad_pec=0;
}
int main(void) {
    struct max30102_device max={&bus,(void *)1};
    struct sht30_device sht={&bus,(void *)1,0x44};
    rt_uint32_t red[8],ir[8];rt_uint16_t count;
    reset();count=99;CHECK(max30102_read_batch(&max,red,ir,8,&count)==0 && count==0 && fifo_reads==0);CHECK(locks==releases);
    CHECK(max30102_read_fifo(&max,red,ir)==-RT_EEMPTY);
    reset();regs[4]=3;regs[6]=30; /* Five samples across the pointer wrap. */
    CHECK(max30102_read_batch(&max,red,ir,2,&count)==0 && count==2);
    CHECK(red[0]==0x10203 && ir[0]==(0x40506 & 0x3ffff));CHECK(locks==releases);
    reset();regs[4]=20;CHECK(max30102_read_batch(&max,red,ir,8,&count)==0 && count==8);
    reset();regs[4]=24;CHECK(max30102_read_batch(&max,red,ir,8,&count)==-RT_EFULL && !count && !fifo_reads);
    reset();regs[5]=1;CHECK(max30102_read_batch(&max,red,ir,8,&count)==-RT_EFULL && !count);
    reset();fail_reg=4;CHECK(max30102_read_batch(&max,red,ir,8,&count)!=0 && !count);CHECK(locks==releases);
    reset();regs[4]=3;short_fifo=1;red[0]=123;CHECK(max30102_read_batch(&max,red,ir,8,&count)!=0 && !count && red[0]==123);CHECK(locks==releases);
    reset();lock_fail=1;CHECK(max30102_reset(&max)!=0 && releases==0 && transfers==0);
    reset();CHECK(max30102_read_batch(&max,red,ir,0,&count)==-RT_EINVAL && !count);
    reset();struct sht30_data data={0,0};CHECK(sht30_read(&sht,&data)==0);
    CHECK(fabsf(data.temperature-25)<0.01f && fabsf(data.humidity-50)<0.01f);
    CHECK(selections==2 && !mux_held && locks==releases);
    for(int fail=1;fail<=2;fail++) {
        reset();select_fail=fail;data.temperature=-999;
        CHECK(sht30_read(&sht,&data)!=0 && data.temperature==-999);
        CHECK(transfers==fail-1 && !mux_held && locks==releases);
    }
    reset();crc_bad=1;data.temperature=-999;CHECK(sht30_read(&sht,&data)!=0 && data.temperature==-999);CHECK(!mux_held && locks==releases);
    reset();send_fail=1;CHECK(sht30_read(&sht,&data)!=0);CHECK(!mux_held && locks==releases);
    reset();recv_fail=1;CHECK(sht30_read(&sht,&data)!=0);CHECK(!mux_held && locks==releases);
    reset();lock_fail=1;CHECK(sht30_reset(&sht)!=0 && !transfers && !releases);
    struct ads1015_device adc={&bus,(void *)1};
    rt_int16_t code;
    reset();code=-999;CHECK(ads1015_read_single_ended(&adc,0,&code)==0 && code==0x123);
    CHECK(adc_polls==1 && adc_conversion_reads==1 && locks==releases);
    reset();adc_ready_after=5;CHECK(ads1015_read_single_ended(&adc,1,&code)==0 && adc_polls==5);
    for(int fault=0;fault<4;fault++) {
        reset();code=-999;
        if(fault==0)adc_ready_after=6;
        if(fault==1)adc_config_fail=1;
        if(fault==2)adc_read_fail=1;
        if(fault==3)adc_send_fail=1;
        CHECK(ads1015_read_single_ended(&adc,0,&code)!=0 && code==-999);
        CHECK(locks==releases);
        if(fault!=2)CHECK(adc_conversion_reads==0);
    }
    reset();lock_fail=1;CHECK(ads1015_read_single_ended(&adc,0,&code)!=0 && transfers==0 && releases==0);
    reset();CHECK(max30102_try_init("i2c1")==NULL); /* wrong part ID */
    reset();regs[REG_PART_ID]=0x15;
    max30102_device_t recovered=max30102_try_init("i2c1");
    CHECK(recovered!=NULL);
    CHECK(max30102_check_config(recovered)==0);
    regs[REG_SPO2_CONFIG]=0;
    CHECK(max30102_check_config(recovered)!=0); /* power reset must be detected */
    regs[REG_SPO2_CONFIG]=0x27;fail_reg=REG_MODE_CONFIG;
    CHECK(max30102_check_config(recovered)!=0);
    CHECK(locks==releases);
    max30102_deinit(recovered);
    reset();mlx90614_device_t mlx=mlx90614_try_init("i2c1");
    CHECK(mlx!=NULL && delays==0); /* 后台探测不得持mux等待300ms */
    float temp=-999;
    CHECK(mlx90614_read_obj_temp(mlx,&temp)==0 && fabsf(temp-26.85f)<0.01f);
    mlx_bad_pec=1;temp=-999;
    CHECK(mlx90614_read_obj_temp(mlx,&temp)!=0 && temp==-999);
    CHECK(locks==releases);mlx90614_deinit(mlx);
    printf("Driver SIL: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
