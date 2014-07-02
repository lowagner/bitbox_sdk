// bootloader

// 2 - second stage bootloader from SD can load a larger bootloader 
// with customizable menu, UI, USB, whatever.


/*
	keep under 64k (error ? )
	cannot intialize USB if already plugged - interactions with bootloader 1 ?
 */

#include <string.h>
#include "stm32f4xx.h" 
#include "system.h" // InitializeSystem
#include "bitbox.h"
#include "flashit.h"

enum {INIT =0, MOUNT=1, OPEN=2, READ=3}; // where we died
#define MAX_FILES 20

void die(int where, int cause);

void led_on() 
{
    GPIOA->BSRRL = 1<<2; 
}

void led_off() 
{
    GPIOA->BSRRH = 1<<2; 
}

void led_init() {
	// init LED GPIO
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN; // enable gpioA (+)
    GPIOA->MODER |= (1 << 4) ; // set pin 8 to be general purpose output
}

void blink(int times, int speed);

FATFS fs32;
// load from sd to RAM
// flash LED, boot



// Code stolen from "matis"
// http://forum.chibios.org/phpbb/viewtopic.php?f=2&t=338
void jump(uint32_t address)
{
    typedef void (*pFunction)(void);

    pFunction Jump_To_Application;

    // variable that will be loaded with the start address of the application
    uint32_t* JumpAddress;
    const uint32_t* ApplicationAddress = (uint32_t*) address;

    // get jump address from application vector table
    JumpAddress = (uint32_t*) ApplicationAddress[1];

    // load this address into function pointer
    Jump_To_Application = (pFunction) JumpAddress;

    // reset all interrupts to default
    // chSysDisable();

    // Clear pending interrupts just to be on the safe side
    //SCB_ICSR = ICSR_PENDSVCLR;

    // Disable all interrupts
    int i;
    for (i = 0; i < 8; i++)
            NVIC->ICER[i] = NVIC->IABR[i];

    // set stack pointer as in application's vector table
    __set_MSP((u32)(ApplicationAddress[0]));
    Jump_To_Application();
}


// ---------------- die : should be integrated to kerne + emulator (as window title by example)

#define WAIT_TIME 168000000/128 // quick ticks 
void wait(int k) 
{
	for (volatile int i=0;i<k*WAIT_TIME;i++) {};
}

void blink(int times, int speed)
{
	for (int i=0;i<times;i++) 
		{
			led_on();wait(speed);
			led_off();wait(speed);
		}
}

void die(int where, int cause)
{
	for (;;)
	{
		blink(where,2);
		wait(4);
		blink(cause,1);
		wait(4);
	}
}

extern const uint8_t font_data [256][16];
char vram_char  [30][80];

int nb_files;
char filenames[MAX_FILES][13]; // 8+3 +. + 1 chr0

#define ICON_W 128 // read from file ?
#define ICON_SIZE (ICON_W*ICON_W/8) // 128x128 1 bit data

int icon_x, icon_y;
uint8_t icon_data[ICON_SIZE]; // 2KB b&w 128x128 data 
// pixel on(i,j) = data[i*16+j/8]&(1<<(7-j%8)))
FIL file;

void print_at(int column, int line, const char *msg)
{
	strcpy(&vram_char[line][column],msg); 
}

// draws an empty window at this position, asserts x1<x2 && y1<y2
// replace with full ascii art ?
void window (int x1, int y1, int x2, int y2 )
{
	for (int i=x1+1;i<x2;i++) 
	{
		vram_char[y1][i]='\xCD';
		vram_char[y2][i]='\xCD';
	}
	for (int i=y1+1;i<y2;i++) 
	{
		vram_char[i][x1]='\xBA';
		vram_char[i][x2]='\xBA';
	}
	vram_char[y1][x1] ='\xC9';
	vram_char[y1][x2] ='\xBB'; 
	vram_char[y2][x1] ='\xC8'; 
	vram_char[y2][x2] ='\xBC'; 
}

void list_roms()
{

    FRESULT res;
    FILINFO fno;
    DIR dir;
    
    char *fn;   /* This function is assuming non-Unicode cfg. */
#if _USE_LFN
    static char lfn[_MAX_LFN + 1];   /* Buffer to store the LFN */
    fno.lfname = lfn;
    fno.lfsize = sizeof lfn;
#endif


    res = f_opendir(&dir, "");                       /* Open the root directory */
    if (res == FR_OK) {
        for (nb_files=0;nb_files<MAX_FILES;) {
            res = f_readdir(&dir, &fno);                   /* Read a directory item */
            if (res != FR_OK || fno.fname[0] == 0) break;  /* Break on error or end of dir */
            if (fno.fname[0] == '.') continue;             /* Ignore dot entry */
#if _USE_LFN
            fn = *fno.lfname ? fno.lfname : fno.fname;
#else
            fn = fno.fname;
#endif
            if (!(fno.fattrib & AM_DIR)) {                    /* It is a directory : do nothing */
                strcpy(filenames[nb_files],fn);
                nb_files +=1;
            }
        }
        if (res != FR_OK) {
	        print_at(5,20,"Error reading directory !");
    	    vram_char[21][5] = '0'+res;
        } 
        f_closedir(&dir);
    } else {
        print_at(5,20,"Error opening directory !");
        vram_char[21][5] = '0'+res;
    }

}
 
// read icon PBM data to memory
int read_icon(const char *filename)
{
	char c;
	FRESULT res;
	unsigned int b_read;
	
	res = f_open (&file, filename, FA_READ);
	if (res != FR_OK)
		return res;

	// header
	for (int i=0;i<3;i++) {
		res = f_read (&file, &c, 1, &b_read);
		if (res != FR_OK) 
			return res;
		if (c!="P4\n"[i])
			return 1000; // bad header
	}
	 
	// skip one non-comment line  
	int lines=0;
	int cmt=0;
	do {
		res = f_read (&file, &c, 1,&b_read); // read first line char
		cmt = (c=='#'); // asserts no empty line
		while (c!='\n') 
			f_read (&file, &c, 1, &b_read);
			// XXX check size 128
		if (!cmt) lines += 1; // in a comment ?
	} while (lines < 1 || cmt); // skip lines
	 
	res=f_read(&file,icon_data,ICON_SIZE,&b_read);
	f_close(&file);
	return res;
}
 
char *HEX_Digits;

void game_init() {
	// CB->VTOR=SRAM1_BASE;
	// HACK, FIXME with a proper init of the interrupt table

	flash_init();

	window(2,2,78,4);
	print_at(5,3, " BITBOX bootloader \x01 Hi ! Here are the current files");

    // init FatFS
	memset(&fs32, 0, sizeof(FATFS));
	FRESULT r = f_mount(&fs32,"",1); // mount now
	if (r != FR_OK) {
		print_at(5,20,"Cannot mount disk");
		die(MOUNT,r); 
	}
	
	memset(icon_data, 0x55, sizeof(icon_data));
	icon_x = 400;
	r=read_icon("bitbox.pbm");
	if (r==FR_OK) 
		icon_y=200; 
	else {
		vram_char[16][1] = HEX_Digits[r&0xf];
		vram_char[16][0] = HEX_Digits[(r>>4)&0xf];
		icon_y=1024; // don't display it now
	}

	list_roms();
}


int selected;
int x =5,y=10 , dir_x=1, dir_y=1;
char old_val=' ';
void game_frame() 
{
	// handle input 
	if (GAMEPAD_PRESSED(0,down) && vga_frame%4==0)
		selected +=1;
	if (selected>nb_files) selected=0;

	if (GAMEPAD_PRESSED(0,up) && vga_frame%4==0)
		selected -=1;
	if (selected<0) selected=nb_files;
	
	// XXX try to read game icon if selected is different

	if (GAMEPAD_PRESSED(0,start) || GAMEPAD_PRESSED(0,A))
	{
		print_at(5,20,"Goldorak GO ! Flashing ");
		print_at(29,20,filenames[selected]);

		if (f_open(&file,filenames[selected],FA_READ)==FR_OK)
		{
			flash_start_write(&file);
		} else {
			print_at(5,20,"Error reading ");
			print_at(20,20,filenames[selected]);
		}		
	}

	memset(&vram_char[21][5],' ',30);
	strcpy(&vram_char[21][5],flash_message);

	// update_display
	for (int i=0;i<nb_files;i++)
	{
		print_at(10,i+10,filenames[i]);		
		// cursor ?
		vram_char[10+i][8]=(i==selected)?0x10:' ';
		vram_char[10+i][25]=(i==selected)?0x11:' ';
	}

	if (vga_frame%2 == 0 ) {
		// bounce guy
		vram_char[y][x]=old_val;
		if (x==59) dir_x = -1;
		if (x==0)  dir_x = 1;

		if (y==6)  dir_y = 1;
		if (y==29) dir_y = -1;

		x += dir_x;
		y += dir_y;
		old_val = vram_char[y][x];
		vram_char[y][x] = '\x02';
	}

	flash_frame(); // at the end to let it finish 
}

extern const uint16_t bg_data[256];

void game_line() 
{
	static uint32_t lut_data[4];
	// text mode

	// bg effect, just because
	uint16_t line_color = bg_data[(vga_frame+vga_line)%256];
	lut_data[0] = 0;
	lut_data[1] = line_color<<16;
	lut_data[2] = (uint32_t) line_color;
	lut_data[3] = line_color * 0x10001;
	

	uint32_t *dst = (uint32_t *) draw_buffer;
	char c;

	for (int i=0;i<80;i++) // column char
	{
		c = font_data[vram_char[vga_line / 16][i]][vga_line%16];
		// draw a character on this line

		*dst++ = lut_data[(c>>6) & 0x3];
		*dst++ = lut_data[(c>>4) & 0x3];
		*dst++ = lut_data[(c>>2) & 0x3];
		*dst++ = lut_data[(c>>0) & 0x3];
	}

	// overdraw icon, 16 bytes data for 128 lines. align icon by 2 pixels.
	if (vga_line>icon_y && vga_line<icon_y+128)
	{
		dst = (uint32_t*) &draw_buffer[icon_x & ~1]; // align 2 pixels
		uint8_t *src = (uint8_t *)&icon_data[(vga_line-icon_y)*16];

		for (int i=0;i<16;i++) // 16*8=128
		{
			// draw a byte worth of pixels
			*dst++ = lut_data[(*src>>6) & 0x3];
			*dst++ = lut_data[(*src>>4) & 0x3];
			*dst++ = lut_data[(*src>>2) & 0x3];
			*dst++ = lut_data[(*src>>0) & 0x3];

			src++; // next byte
		}
	}
}

void game_snd_buffer(uint16_t *buffer, int len) {} // beeps ?

