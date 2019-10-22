#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>
#include <poll.h>

struct {
    unsigned char channel;
    unsigned char number;
    uint16_t code;
} knobs[] = {
    {0x00, 0x1, ABS_X},
    {0x00, 0x2, ABS_Y},
    {0x00, 0x3, ABS_Z},
    {0x00, 0x4, ABS_RX},
    {0x00, 0x5, ABS_RY},
    {0x00, 0x6, ABS_RZ},
    {0x00, 0x7, ABS_HAT0X},
    {0x00, 0x8, ABS_HAT0Y},
    {0xFF, 0xFF, 0} //sentinal
};

struct {
    unsigned char channel;
    unsigned char key;
    uint16_t code;
    uint16_t lednum;
    bool ledstate;
} buttons[] = {
    {0x00, 0x24, BTN_0, 0},
    {0x00, 0x25, BTN_1, 1},
    {0x00, 0x26, BTN_2, 2},
    {0x00, 0x27, BTN_3, 3},
    {0x00, 0x28, BTN_4, 4},
    {0x00, 0x29, BTN_5, 5},
    {0x00, 0x2a, BTN_6, 6},
    {0x00, 0x2b, BTN_7, 7},
    {0xFF, 0xFF, 0} //sentinal
};

void usage(const char * const name) {
    printf("usage: %s <mididevice>\n", name);
}

int main(int argc, char *argv[]) {

    int midi_fd;
    int input_fd = open("/dev/uinput", O_RDWR);
    bool verbose = false;
    bool ledhack = false;

    if (input_fd < 0) {
        printf("Cannot open /dev/uinput: %m\n");
        return -1;
    }
    
    {
        int opt;
        while ((opt = getopt(argc, argv, "vl")) != -1) {
            switch (opt) {
            case 'v':
                verbose = true;
                break;
            case 'l':
                ledhack = true;
                break;
            default: /* '?' */
                usage(argv[0]);
                return -1;
            }
        }
    }

    if (optind != argc - 1) {
        usage(argv[0]);
        return -1;
    }

    
    midi_fd = open(argv[optind], O_RDWR);
    
    if (verbose) {
        printf("MIDI FD (%s), %d\n", argv[optind], midi_fd);
        printf("INPUT FD (%s), %d\n", "/dev/uinput", input_fd);
    }

    //set up input fd;
    ioctl(input_fd, UI_SET_EVBIT, EV_KEY);
    if (ledhack) {
        ioctl(input_fd, UI_SET_EVBIT, EV_LED);
    }
    for (int i = 0; buttons[i].channel != 0xFF; i++) {
        if (ledhack) {
            ioctl(input_fd, UI_SET_LEDBIT, buttons[i].lednum);
        }
        ioctl(input_fd, UI_SET_KEYBIT, buttons[i].code);
    }
    ioctl(input_fd, UI_SET_EVBIT, EV_ABS);
    for (int i = 0; knobs[i].channel != 0xFF; i++) {
        struct uinput_abs_setup setup = {};
        
        ioctl(input_fd, UI_SET_ABSBIT, knobs[i].code);

        setup.code = knobs[i].code;
        setup.absinfo.value = 0;
        setup.absinfo.minimum = 0;
        setup.absinfo.maximum = 0x7f;

        ioctl(input_fd, UI_ABS_SETUP, &setup);
    }

    {
        struct uinput_setup setup = {};
        setup.id.bustype = BUS_VIRTUAL;
        setup.id.vendor = 0;
        setup.id.product = 0;
        setup.id.version = 0;
        strncpy(setup.name, "MIDI ADAPTOR", sizeof(setup.name));
        ioctl(input_fd, UI_DEV_SETUP, &setup);
        ioctl(input_fd, UI_DEV_CREATE);
    }
    
    for (;;) {
        enum {
            MIDI_FD = 0,
            INPUT_FD,
            NUM_FDS
        };
        struct pollfd fds[] = {
            {
                midi_fd,
                POLLIN,
                0
            },
            {
                input_fd,
                POLLIN,
                0
            }
        };
        
        poll(fds, NUM_FDS, 1000);

        if (fds[MIDI_FD].revents & POLLIN) {
            unsigned char c[3];
            int len;
            const char *type = "";
            //this while loop gets us in sync (if we've lost it). we don't support running status (yet)
            do {
                read(midi_fd, &c[0], 1);
            } while ((c[0] & 0x80) == 0);
            
            switch(c[0] & 0xF0) {            
            case 0x80:
                type = "note off";
                len = 2;
                break;
            case 0x90:
                type = "note on";
                len = 2;
                break;
            case 0xA0:
                type = "Polyphonic Key Pressure";
                len = 2;
                break;
            case 0xB0:
                type = "Controller Change";
                len = 2;
                break;
            case 0xC0:
                type = "Program Change";
                len = 1;
                break;
            case 0xD0:
                type = "Channel Key Pressure (aftertouch)";
                len = 1;
                break;
            case 0xE0:
                type = "Pitch Bend";
                len = 2;
                break;
            default:
                continue;
            }

            read(midi_fd, &c[1], len);
        
            switch(c[0] & 0xF0) {            
            case 0x80:
                type = "note off";
                len = 2;
                for (int i = 0; buttons[i].channel != 0xFF; i++) {
                    if ((buttons[i].channel == (c[0] & 0x0F)) &&
                        (buttons[i].key == c[1])) {
                        struct input_event ie;
                        ie.type = EV_KEY;
                        ie.code = buttons[i].code;
                        ie.value = 0;
                        //time values are ignored
                        ie.time.tv_sec = 0;
                        ie.time.tv_usec = 0;
                        write(input_fd, &ie, sizeof(ie));
                        if (ledhack) {
                            unsigned char o[3] = {
                                buttons[i].ledstate?0x90:0x80,
                                buttons[i].key,
                                0x7f
                            };
                            write(midi_fd, o, sizeof(o));
                        }
                    }
                }
                break;
            case 0x90:
                type = "note on";
                len = 2;
                for (int i = 0; buttons[i].channel != 0xFF; i++) {
                    if ((buttons[i].channel == (c[0] & 0x0F)) &&
                        (buttons[i].key == c[1])) {
                        struct input_event ie;
                        ie.type = EV_KEY;
                        ie.code = buttons[i].code;
                        ie.value = !!(c[2] != 0);
                        //time values are ignored
                        ie.time.tv_sec = 0;
                        ie.time.tv_usec = 0;
                        write(input_fd, &ie, sizeof(ie));
                        if (ledhack) {
                            unsigned char o[3] = {
                                buttons[i].ledstate?0x90:0x80,
                                buttons[i].key,
                                0x7f
                            };
                            write(midi_fd, o, sizeof(o));
                        }
                    }
                }
                break;
            case 0xA0:
                type = "Polyphonic Key Pressure";
                len = 2;
                break;
            case 0xB0:
                type = "Controller Change";
                len = 2;
                for (int i = 0; knobs[i].channel != 0xFF; i++) {
                    if ((knobs[i].channel == (c[0] & 0x0F)) &&
                        (knobs[i].number == c[1])) {
                        struct input_event ie;
                        ie.type = EV_ABS;
                        ie.code = knobs[i].code;
                        ie.value = c[2];
                        //time values are ignored
                        ie.time.tv_sec = 0;
                        ie.time.tv_usec = 0;
                        write(input_fd, &ie, sizeof(ie));
                    }
                }
                break;
            case 0xC0:
                type = "Program Change";
                len = 1;
                break;
            case 0xD0:
                type = "Channel Key Pressure (aftertouch)";
                len = 1;
                break;
            case 0xE0:
                type = "Pitch Bend";
                len = 2;
                break;
            default:
                continue;
            }
        
            {
                struct input_event ie;
                ie.type = EV_SYN;
                ie.code = SYN_REPORT;
                ie.value = 0;
                //time values are ignored
                ie.time.tv_sec = 0;
                ie.time.tv_usec = 0;
                write(input_fd, &ie, sizeof(ie));
            }

            if (verbose) {
                if (len == 1) {
                    printf("%s %02x %02x\n", type, c[0] & 0xFF, c[1] & 0xFF);
                }
                else {
                    printf("%s %02x %02x %02x\n", type, c[0] & 0xFF, c[1] & 0xFF, c[2] & 0xFF);
                }
                fflush(stdout);
            }
        }
        
        if (fds[INPUT_FD].revents & POLLIN) {
            struct input_event ie;
            read(input_fd, &ie, sizeof(ie));

            if (ledhack) {
                if (ie.type == EV_LED) {
                    for (int i = 0; buttons[i].channel != 0xFF; i++) {
                        if (buttons[i].lednum == ie.code) {
                            buttons[i].ledstate = !!ie.value;
                            {
                                unsigned char o[3] = {
                                buttons[i].ledstate?0x90:0x80,
                                buttons[i].key,
                                0x7f
                                };
                                write(midi_fd, o, sizeof(o));
                            }
                        }
                    }
                }
            }
        }
    }
    
    return 0;
}
