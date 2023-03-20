// TODO: Add explanation of the file here.

#include <linux/module.h>
#include <linux/miscdevice.h>		// for misc-driver calls.
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/slab.h> // For kmalloc and kfree.
#include <linux/kfifo.h>

#define MC_DEVICE_FILE  "morse-code"
#define LOG_PREFIX "morse_code_driver:"
#define ALL_WHITESPACE -1
#define MC_FIFO_MAX_SIZE 512

#define DOT_TIME_MS 200
#define DASH_TIME_MS (3 * DOT_TIME_MS)
#define DOT_DASH_SEP_TIME_MS DOT_TIME_MS
#define LETTER_SEP_TIME_MS DASH_TIME_MS
#define WORD_SEP_TIME_MS (7 * DOT_TIME_MS)

/// Return the number of elements in the array x.
#define NUM_ELEMS(x) sizeof((x)) / sizeof((x)[0])

/// Return 1 if ch is a letter and 0 otherwise.
#define IS_LETTER(ch) (('A' <= (ch) && (ch) <= 'Z') || ('a' <= (ch) && (ch) <= 'z'))

// This macro assumes that IS_LETTER(ch) == 1
#define TO_LOWERCASE(ch) (((ch) >= 'a') ? (ch) : (ch) + 'a' - 'A');

// This macro assumes that ch is a lowercase letter.
#define MORSE_BITS_INDEX(ch) ((ch) - 'a')

// Morse Code Encodings (from http://en.wikipedia.org/wiki/Morse_code)
//   Encoding created by Brian Fraser. Released under GPL.
//
// Encoding description:
// - msb to be output first, followed by 2nd msb... (left to right)
// - each bit gets one "dot" time.
// - "dashes" are encoded here as being 3 times as long as "dots". Therefore
//   a single dash will be the bits: 111.
// - ignore trailing 0's (once last 1 output, rest of 0's ignored).
// - Space between dashes and dots is one dot time, so is therefore encoded
//   as a 0 bit between two 1 bits.
//
// Example:
//   R = dot   dash   dot       -- Morse code
//     =  1  0 111  0  1        -- 1=LED on, 0=LED off
//     =  1011 101              -- Written together in groups of 4 bits.
//     =  1011 1010 0000 0000   -- Pad with 0's on right to make 16 bits long.
//     =  B    A    0    0      -- Convert to hex digits
//     = 0xBA00                 -- Full hex value (see value in table below)
//
// Between characters, must have 3-dot times (total) of off (0's) (not encoded here)
// Between words, must have 7-dot times (total) of off (0's) (not encoded here).
//
static unsigned short morse_codes[] = {
		0xB800,	// A 1011 1
		0xEA80,	// B 1110 1010 1
		0xEBA0,	// C 1110 1011 101
		0xEA00,	// D 1110 101
		0x8000,	// E 1
		0xAE80,	// F 1010 1110 1
		0xEE80,	// G 1110 1110 1
		0xAA00,	// H 1010 101
		0xA000,	// I 101
		0xBBB8,	// J 1011 1011 1011 1
		0xEB80,	// K 1110 1011 1
		0xBA80,	// L 1011 1010 1
		0xEE00,	// M 1110 111
		0xE800,	// N 1110 1
		0xEEE0,	// O 1110 1110 111
		0xBBA0,	// P 1011 1011 101
		0xEEB8,	// Q 1110 1110 1011 1
		0xBA00,	// R 1011 101
		0xA800,	// S 1010 1
		0xE000,	// T 111
		0xAE00,	// U 1010 111
		0xAB80,	// V 1010 1011 1
		0xBB80,	// W 1011 1011 1
		0xEAE0,	// X 1110 1010 111
		0xEBB8,	// Y 1110 1011 1011 1
		0xEEA0	// Z 1110 1110 101
};

static char whitespaces[] = {
	' ',
	'\t',
	'\r',
	'\n'
};

static DECLARE_KFIFO(mc_fifo, char, MC_FIFO_MAX_SIZE);

static int to_morse(const char* src, int len);

/******************************************************
 * Callbacks
 ******************************************************/
static int mc_open(struct inode *inode, struct file *file)
{
	printk(KERN_DEBUG "morse_code_driver: In mc_open()\n");
	return 0;  // Success
}

static int mc_close(struct inode *inode, struct file *file)
{
	printk(KERN_DEBUG "morse_code_driver: In mc_close()\n");
	return 0;  // Success
}

static ssize_t mc_read(struct file *file, char *buff, size_t count, loff_t *ppos)
{
	int res;
	int num_bytes_copied = 0;

	printk(KERN_DEBUG "morse_code_driver: In mc_read()\n");

	res = kfifo_to_user(&mc_fifo, buff, count, &num_bytes_copied);

	if (res == -EFAULT) return -EFAULT;

	return num_bytes_copied;
}

static ssize_t mc_write(struct file *file, const char *buff, size_t count, loff_t *ppos)
{
    /*
     Multiple things to do here, in no particular order:
     1) Honour the contract of this function (maintain ppos, return correct count)
     2) Translate from ASCI to morse code.
     3) Use translation to flash LED
     4) Copy translation to kfifo
    */

    char* user_chars;

	printk(KERN_DEBUG "morse_code_driver: In mc_write()\n");

    user_chars = kmalloc(count, GFP_KERNEL);
    if (copy_from_user(user_chars, buff, count)) {
        kfree(user_chars);
        return -EFAULT;
    }

	if (to_morse(user_chars, count) != count) {
		printk(KERN_ERR "%s ERROR: to_morse() failed.\n", LOG_PREFIX);
	}

    kfree(user_chars);
	*ppos += count;

	return count;
}

/******************************************************
 * Helper functions for callbacks
 ******************************************************/
/// Returns 1 if char is a whitespace character and 0 otherwise.
static int is_whitespace(char ch)
{
	int i;
	for (i = 0; i < NUM_ELEMS(whitespaces); i++) {
		if (whitespaces[i] == ch) return 1;
	}
	return 0;
}

/**
 * Set out_first and out_last to indicate a substring of src which has leading and trailing whitespace stripped off it.
 * @param src character buffer
 * @param len number of characters in src
 * @param out_first output parameter -- pointer to index indicating the location of the first non-whitespace character
 *      -- if there are no non-whitespace characters, *out_first == ALL_WHITESPACE == -1
 * @param out_last output parameter -- pointer to index indicating the location of the last non-whitespace character
 *      -- if there are no non-whitespace characters, *out_last == ALL_WHITESPACE == -1
 */
static void strip_whitespace(const char* src, int len, int* out_first, int* out_last)
{
	int first = 0;
	int last = len - 1;

	// Find first.
	for (; first <= last && is_whitespace(src[first]); first++);

	// All characters are whitespace.
	if (first == len) {
		*out_first = ALL_WHITESPACE;
		*out_last = ALL_WHITESPACE;
		return;
	}

	// Find last.
	for (; first < last && is_whitespace(src[last]); last--);

	*out_first = first;
	*out_last = last;
}

/// Print the ASCII character codes for each character in src.
static void print_ascii(const char* src, int len)
{
	int i;
	printk(KERN_DEBUG "%s ASCII: ", LOG_PREFIX);
	for (i = 0; i < len; i++) {
		printk(KERN_DEBUG "%s %d\n", LOG_PREFIX, (int) src[i]);
	}
}

static void put_dot(int is_first)
{
	printk(KERN_DEBUG "%s put_dot().\n", LOG_PREFIX);
	if (!is_first) {
		// TODO: LED off.
		msleep(DOT_DASH_SEP_TIME_MS);
	}
	kfifo_put(&mc_fifo, '.');
	// TODO: LED on.
	msleep(DOT_TIME_MS);
}

static void put_dash(int is_first)
{
	printk(KERN_DEBUG "%s put_dash().\n", LOG_PREFIX);
	if (!is_first) {
		// TODO: LED off.
		msleep(DOT_DASH_SEP_TIME_MS);
	}
	kfifo_put(&mc_fifo, '-');
	// TODO: LED on.
	msleep(DASH_TIME_MS);
}

static void put_space(void)
{
	printk(KERN_DEBUG "%s put_space().\n", LOG_PREFIX);
	kfifo_put(&mc_fifo, ' ');
	// TODO: LED off.
	msleep(LETTER_SEP_TIME_MS);
}

static void put_word_sep(void)
{
	int i;
	printk(KERN_DEBUG "%s put_word_sep().\n", LOG_PREFIX);
	for (i = 0; i < 2; i++) {
		kfifo_put(&mc_fifo, ' ');
	}
	// TODO: LED off.
	msleep(WORD_SEP_TIME_MS);
}

static void put_newline(int is_first)
{
	printk(KERN_DEBUG "%s put_newline().\n", LOG_PREFIX);
	if (!is_first) {
		// TODO: LED off.
		msleep(DOT_DASH_SEP_TIME_MS);
	}
	kfifo_put(&mc_fifo, '\n');
}

/**
 * Translate from ASCII to morse code and place result in mc_fifo. Returns 0 if no characters were processed due to
 * some error, and len otherwise.
 * @param src the ASCI buffer
 * @param len the number of characters in src
 */
static int to_morse(const char* src, int len)
{
	int first;
	int last;
	int i;
	char* null_termed_src = NULL;
	char* substring = NULL;
	size_t subsize = 0;
	if (len <= 0) {
		printk(KERN_ERR "%s ERROR: Bad argument len == %d.\n", LOG_PREFIX, len);
		return 0;
	}

	// Print src.
	null_termed_src = kmalloc(len + 1, GFP_KERNEL);
	if (null_termed_src == NULL) {
		printk(KERN_WARNING "%s WARNING: kmalloc failed.\n", LOG_PREFIX);
	}
	else {
		memcpy(null_termed_src, src, len);
		null_termed_src[len] = '\0';
		printk(KERN_DEBUG "%s src = '%s'.\n", LOG_PREFIX, null_termed_src);
		print_ascii(null_termed_src, len + 1);
		kfree(null_termed_src);
	}

	// Set indices first and last to indicate the range of a substring of src that has been stripped of leading and
	// trailing whitespace characters.
	strip_whitespace(src, len, &first, &last);
	if (first == ALL_WHITESPACE) {
		printk(KERN_DEBUG "%s src is all whitespace.\n", LOG_PREFIX);
		return len;
	}
	if (last == ALL_WHITESPACE) {
		printk(KERN_ERR "%s ERROR: first != ALL_WHITESPACE, but last == ALL_WHITESPACE.\n", LOG_PREFIX);
		return 0;
	}
	// Print the substring.
	subsize = last - first + 2;
	substring = kmalloc(subsize, GFP_KERNEL);
	if (substring == NULL) {
		printk(KERN_WARNING "%s WARNING: kmalloc failed.\n", LOG_PREFIX);
	}
	else {
		memcpy(substring, &src[first], subsize - 1);
		substring[subsize - 1] = '\0';
		printk(KERN_DEBUG "%s substring = '%s'.\n", LOG_PREFIX, substring);
		print_ascii(substring, subsize);
		kfree(substring);
	}

	// Translate the substring into morse code. Place each translated character onto mc_fifo.
	for (i = first; i <= last; i++) {
		char ch = src[i];
		unsigned short morse_bits;
		char k;
		char num_consecutive_ones;
		char num_consecutive_zeros;
		printk(KERN_DEBUG "%s ch = %c.\n", LOG_PREFIX, ch);
		if (IS_LETTER(ch)){
			ch = TO_LOWERCASE(ch);
			printk(KERN_DEBUG "%s TO_LOWERCASE(ch) = %c.\n", LOG_PREFIX, ch);
			morse_bits = morse_codes[MORSE_BITS_INDEX(ch)];
			num_consecutive_ones = 0;
			num_consecutive_zeros = 0;
			printk(KERN_DEBUG "%s morse_bits = 0x%x.\n", LOG_PREFIX, morse_bits);
			for (k = sizeof(morse_bits) * 8 - 1; k >= 0; k--) {
				unsigned char bit = ((morse_bits & (1 << k)) >> k);
				printk(KERN_DEBUG "%s bit #%d = %d.\n", LOG_PREFIX, k, bit);
				if (bit == 1) {
					num_consecutive_ones++;
					num_consecutive_zeros = 0;
				}
				else {
					switch (num_consecutive_ones) {
						case 0:
							// Assumed to be part of trailing sequence of zeros.
							num_consecutive_zeros++;
							if (num_consecutive_zeros >= 2) {
								// We've reached the end of the code.
								if (i != last) {
									put_space();
								}
							}
							break;
						case 1:
							put_dot((i == first));
							break;
						case 3:
							put_dash((i == first));
							break;
						default:
							printk(KERN_WARNING "%s WARNING: Invalid morse code 0x%x for character '%c'.\n",
								   LOG_PREFIX,
								   morse_bits,
								   ch);
							break;
					}
					if (num_consecutive_zeros >= 2) break;
					num_consecutive_ones = 0;
				}
			}
		}
		else if (ch == ' ') {
			put_word_sep();
		}
		if (i == last) {
			put_newline((i == first));
		}
	}

	return len;
}

// TODO: I think this is unneeded.
// static long mc_unlocked_ioctl (struct file *file, unsigned int cmd, unsigned long arg)
// {
// 	printk(KERN_DEBUG "morse_code_driver: In mc_unlocked_ioctl()\n");
// 	return 0;  // Success
// }

/******************************************************
 * Misc support
 ******************************************************/
// Callbacks:  (structure defined in /linux/fs.h)
struct file_operations mc_fops = {
	.owner    =  THIS_MODULE,
	.open     =  mc_open,
	.release  =  mc_close,
	.read     =  mc_read,
	.write    =  mc_write
	// .unlocked_ioctl =  mc_unlocked_ioctl
};

// Character Device info for the Kernel:
static struct miscdevice mc_miscdevice = {
		.minor    = MISC_DYNAMIC_MINOR,         // Let the system assign one.
		.name     = MC_DEVICE_FILE,             // /dev/.... file.
		.fops     = &mc_fops                    // Callback functions.
};


/******************************************************
 * Driver initialization and exit:
 ******************************************************/
static int __init morse_code_driver_init(void)
{
	int res;
    printk(KERN_DEBUG "----> morse_code_driver_init() -- '/dev/%s'.\n", MC_DEVICE_FILE);
	res = misc_register(&mc_miscdevice);
	INIT_KFIFO(mc_fifo);

	return res;
}

static void __exit morse_code_driver_exit(void)
{
    printk(KERN_DEBUG "<---- morse_code_driver_exit().\n");
	misc_deregister(&mc_miscdevice);
}
// Link our init/exit functions into the kernel's code.
module_init(morse_code_driver_init);
module_exit(morse_code_driver_exit);

// Information about this module:
MODULE_AUTHOR("Benjamin Howard");
MODULE_DESCRIPTION("A driver to translate ASCII text into morse code!");
MODULE_LICENSE("GPL"); // Important to leave as GPL.
