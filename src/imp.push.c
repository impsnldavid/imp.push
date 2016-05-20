#include "libusb.h"
#include "jit.common.h"

// Constants
#define ABLETON_VENDOR_ID 0x2982
#define PUSH2_PRODUCT_ID 0x1967
#define PUSH2_BULK_EP_OUT 0x01
#define PUSH2_TRANSFER_TIMEOUT 1000

#define PUSH2_DISPLAY_WIDTH 960
#define PUSH2_DISPLAY_HEIGHT 160
#define PUSH2_DISPLAY_LINE_BUFFER_SIZE 2048
#define PUSH2_DISPLAY_LINE_GUTTER_SIZE 128
#define PUSH2_DISPLAY_LINE_DATA_SIZE PUSH2_DISPLAY_LINE_BUFFER_SIZE - 
#define PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE 16384
#define PUSH2_DISPLAY_IMAGE_BUFFER_SIZE PUSH2_DISPLAY_LINE_BUFFER_SIZE * PUSH2_DISPLAY_HEIGHT
#define PUSH2_DISPLAY_MESSAGES_PER_IMAGE (PUSH2_DISPLAY_LINE_BUFFER_SIZE * PUSH2_DISPLAY_HEIGHT) / PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE

#define PUSH2_DISPLAY_SHAPING_PATTERN_1 0xE7
#define PUSH2_DISPLAY_SHAPING_PATTERN_2 0xF3
#define PUSH2_DISPLAY_SHAPING_PATTERN_3 0xE7
#define PUSH2_DISPLAY_SHAPING_PATTERN_4 0xFF

#define PUSH2_DISPLAY_FRAMERATE 60

const uint8_t PUSH2_DISPLAY_FRAME_HEADER[] =
{ 0xFF, 0xCC, 0xAA, 0x88,
0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00 };


// Struct definition
typedef struct _imp_push
{
	t_object object;

	t_systhread thread;
	t_systhread_mutex mutex;
	t_bool isThreadCancel;

	t_uint8* draw_buffer;
	t_uint8* send_buffer;

	libusb_device_handle* device;
	BOOL is_matrix_received;

} t_imp_push;

// Prototypes
BEGIN_USING_C_LINKAGE
t_jit_err imp_push_init();
t_imp_push* imp_push_new();
void imp_push_free(t_imp_push* x);
t_jit_err imp_push_matrix_calc(t_imp_push* x, void* inputs, void* outputs);
void imp_push_copyandmask_buffer(t_imp_push* x);
void* imp_push_threadproc(t_imp_push *x);
libusb_device_handle* imp_push_open_device(t_imp_push* x);
void imp_push_close_device(t_imp_push* x, libusb_device_handle* device_handle);
END_USING_C_LINKAGE

// Globals
static t_class* s_imp_push_class = NULL;

// Class registration
t_jit_err imp_push_init()
{
	long attrflags = JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_USURP_LOW;
	t_jit_object* attr;
	t_jit_object* mop;

	s_imp_push_class = (t_class*)jit_class_new("imp_push", (method)imp_push_new, (method)imp_push_free, sizeof(t_imp_push), 0);

	// add matrix operator (mop)
	mop = (t_jit_object *)jit_object_new(_jit_sym_jit_mop, 1, 0);
	jit_mop_single_type(mop, _jit_sym_char);
	jit_mop_single_planecount(mop, 4);
	
	t_atom args[2];
	jit_atom_setlong(args, PUSH2_DISPLAY_WIDTH);
	jit_atom_setlong(args + 1, PUSH2_DISPLAY_HEIGHT);

	void* input = jit_object_method(mop, _jit_sym_getinput, 1);
	jit_object_method(input, _jit_sym_mindim, 2, &args);
	jit_object_method(input, _jit_sym_maxdim, 2, &args);
	jit_object_method(input, _jit_sym_ioproc, jit_mop_ioproc_copy_adapt);
	
	jit_class_addadornment(s_imp_push_class, mop);

	// add methods
	jit_class_addmethod(s_imp_push_class, (method)imp_push_matrix_calc, "matrix_calc", A_CANT, 0);

	// finalize class
	jit_class_register(s_imp_push_class);
	return JIT_ERR_NONE;
}


// **************************************************************************************************************************

t_imp_push* imp_push_new()
{
	t_imp_push* x = NULL;

	x = (t_imp_push*)jit_object_alloc(s_imp_push_class);
	if (x)
	{
		systhread_mutex_new(&x->mutex, 0);

		x->draw_buffer = sysmem_newptrclear(PUSH2_DISPLAY_IMAGE_BUFFER_SIZE);
		x->send_buffer = sysmem_newptrclear(PUSH2_DISPLAY_IMAGE_BUFFER_SIZE);
		x->is_matrix_received = false;

		x->device = imp_push_open_device(x);

		systhread_create((method)imp_push_threadproc, x, 0, 0, 0, &x->thread);
	}
	return x;
}

void imp_push_free(t_imp_push* x)
{
	x->isThreadCancel = true; 
	uint* value;
	systhread_join(x->thread, &value);

	systhread_mutex_free(x->mutex);

	if (x->device != NULL)
		imp_push_close_device(x, x->device);

	sysmem_freeptr(x->draw_buffer);
	sysmem_freeptr(x->send_buffer);
}

t_jit_err imp_push_matrix_calc(t_imp_push* x, void* inputs, void* outputs)
{
	t_jit_err			err = JIT_ERR_NONE;
	long				in_savelock;
	t_jit_matrix_info	in_minfo;
	char				*in_bp;
	long				i;
	long				dimcount;
	long				planecount;
	long				dim[JIT_MATRIX_MAX_DIMCOUNT];
	void				*in_matrix;

	in_matrix = jit_object_method(inputs, _jit_sym_getindex, 0);

	if (x && in_matrix)
	{
		in_savelock = (long)jit_object_method(in_matrix, _jit_sym_lock, 1);

		x->is_matrix_received = true;

		jit_object_method(in_matrix, _jit_sym_getinfo, &in_minfo);

		jit_object_method(in_matrix, _jit_sym_getdata, &in_bp);

		if (!in_bp)
		{
			err = JIT_ERR_INVALID_INPUT;
			goto out;
		}

		//get dimensions/planecount
		dimcount = in_minfo.dimcount;
		planecount = in_minfo.planecount;

		uint8_t* src = in_bp;

		uint16_t* dst = (uint16_t*)x->draw_buffer;

		for(int dX = 0; dX < PUSH2_DISPLAY_HEIGHT; ++dX)
		{
			for (int dY = 0; dY < PUSH2_DISPLAY_WIDTH; ++dY)
			{
				*dst++ = (*(src + 1) >> 3) | ((*(src + 2) & 0xFC) << 3) | ((*(src + 3) & 0xF8) << 8);
				src += 4;
			}

			dst += PUSH2_DISPLAY_LINE_GUTTER_SIZE / 2;
		}

		imp_push_copyandmask_buffer(x);
	}
	else
	{
		return JIT_ERR_INVALID_PTR;
	}

out:
	jit_object_method(in_matrix, _jit_sym_lock, in_savelock);
	return err;
}

void imp_push_copyandmask_buffer(t_imp_push* x)
{
	systhread_mutex_lock(x->mutex);

	uint8_t* src = x->draw_buffer;
	uint8_t* dst = x->send_buffer;

	for(int dY = 0; dY < PUSH2_DISPLAY_HEIGHT; ++dY)
	{
		for(int dX = 0; dX < PUSH2_DISPLAY_LINE_BUFFER_SIZE - PUSH2_DISPLAY_LINE_GUTTER_SIZE; dX += 4)
		{
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_1;
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_2;
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_3;
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_4;
		}

		src += PUSH2_DISPLAY_LINE_GUTTER_SIZE;
		dst += PUSH2_DISPLAY_LINE_GUTTER_SIZE;
	}

	systhread_mutex_unlock(x->mutex);
}

void* imp_push_threadproc(t_imp_push *x)
{
	while(!x->isThreadCancel)
	{
		if (x->device != NULL && x->is_matrix_received)
		{
			systhread_mutex_lock(x->mutex);

			int actual_length;

			int result = libusb_bulk_transfer(
				x->device,
				PUSH2_BULK_EP_OUT,
				PUSH2_DISPLAY_FRAME_HEADER,
				sizeof(PUSH2_DISPLAY_FRAME_HEADER),
				&actual_length,
				PUSH2_TRANSFER_TIMEOUT);

			switch(result)
			{
			case LIBUSB_ERROR_TIMEOUT:
			case LIBUSB_ERROR_PIPE:
			case LIBUSB_ERROR_OVERFLOW:
			case LIBUSB_ERROR_NO_DEVICE:

				break;

			case 0:

				break;
			}

			for (int i = 0; i < PUSH2_DISPLAY_MESSAGES_PER_IMAGE; ++i)
			{
				int result = libusb_bulk_transfer(
					x->device,
					PUSH2_BULK_EP_OUT,
					x->send_buffer + (i * PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE),
					PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE,
					&actual_length,
					PUSH2_TRANSFER_TIMEOUT);

				if (result != 0)
					break;
			}

			systhread_mutex_unlock(x->mutex);
		}

		systhread_sleep(1000 / PUSH2_DISPLAY_FRAMERATE);
	}
}

libusb_device_handle* imp_push_open_device(t_imp_push* x)
{
	int result;

	if ((result = libusb_init(NULL)) < 0)
	{
		object_error((t_object*)x, "Failed to initilialize libusb", result);
		return NULL;
	}

	libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_ERROR);

	libusb_device** devices;
	ssize_t count;
	count = libusb_get_device_list(NULL, &devices);
	if (count < 0)
	{
		object_error((t_object*)x, "Failed to get USB device list");
		return NULL;
	}

	libusb_device* device;
	libusb_device_handle* device_handle = NULL;

	for (int i = 0; (device = devices[i]) != NULL; ++i)
	{
		struct libusb_device_descriptor descriptor;
		if ((result = libusb_get_device_descriptor(device, &descriptor)) < 0)
		{
			object_error((t_object*)x, "Failed to get USB device descriptor");
			continue;
		}

		if (descriptor.bDeviceClass == LIBUSB_CLASS_PER_INTERFACE
			&& descriptor.idVendor == ABLETON_VENDOR_ID
			&& descriptor.idProduct == PUSH2_PRODUCT_ID)
		{
			if ((result = libusb_open(device, &device_handle)) < 0)
			{
				object_error((t_object*)x, "Failed to open Push 2 device");
			}
			else if ((result = libusb_claim_interface(device_handle, 0)) < 0)
			{
				object_error((t_object*)x, "Failed to open Push 2 device, may be in use by another application");
				libusb_close(device_handle);
				device_handle = NULL;
			}
			else
			{
				// Successfully opened
				break; 
			}
		}
	}

	libusb_free_device_list(devices, 1);
	return device_handle;
}

void imp_push_close_device(t_imp_push* x, libusb_device_handle* device_handle)
{
	if (device_handle == NULL)
		return;

	libusb_release_interface(device_handle, 0);
	libusb_close(device_handle);
}