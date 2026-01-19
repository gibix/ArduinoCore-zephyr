#include "PDM.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

/* ---- CONFIGURATION ----- */

/* enable disable library debug */
// #define PDM_DEBUG_ENABLED
/* NOTE: quite obvious to note that enabling the debug flag make any sample
 * obtained from the microphone via serial "dirty" because the debug log is
 * inserted into the stream obtained from the microphone. So enable it only if
 * the data from the mic are unimportant */

/* SLAB configuration */
#define SLAB_BLOCK_NUM        4
#define SLAB_ALIGN            4
/* THREAD configuration */
#define PDM_THREAD_STACK_SIZE 1024
#define PDM_THREAD_PRIORITY   7
/* mic power regulator configuration */
/* DT_NODELABEL(mic_pwr) gets the node ID.
 * DT_NODE_HAS_STATUS(..., okay) returns 1 if status is "okay", 0 otherwise. */
#define MIC_PWR_NODE          DT_NODELABEL(mic_pwr)
#if DT_NODE_EXISTS(MIC_PWR_NODE) && DT_NODE_HAS_STATUS(MIC_PWR_NODE, okay)
static const struct device *mic_regulator = DEVICE_DT_GET(MIC_PWR_NODE);
#define MIC_PWR_PRESENT
#endif

/* ---- static local variables ---- */

struct k_mem_slab pdm_slab;
static uint8_t __aligned(4) pdm_slab_buffer[SLAB_BLOCK_SIZE * SLAB_BLOCK_NUM];
static struct k_mutex pdm_mutex;
static void pdm_thread(void *, void *, void *);
static void (*_onReceive)(void) = NULL;
static PDMDoubleBuffer *pdm_db = nullptr;

/* ---- MIC RECEIVING THREAD ---- */

K_THREAD_DEFINE(pdm_tid, PDM_THREAD_STACK_SIZE, pdm_thread, NULL, NULL, NULL, PDM_THREAD_PRIORITY,
				0, 0);

void pdm_thread(void *, void *, void *) {

	void *buffer;
	uint32_t size;

	k_thread_suspend(pdm_tid);

	while (true) {

#ifdef PDM_DEBUG_ENABLED
		Serial.println("[LOG]: Call -> dmic_read");
#endif

		int ret = arduino::pdm_read(&buffer, &size);

		if (ret < 0) {
#ifdef PDM_DEBUG_ENABLED
			Serial.print("[ERR]: dmic_read failed with err = ");
			Serial.println(ret);
#endif
			continue;
		}

		if (pdm_db != nullptr) {
#ifdef PDM_DEBUG_ENABLED
			Serial.print("[LOG]: receiving ");
			Serial.print(size);
			Serial.println(" bytes from mic");
#endif

			k_mutex_lock(&pdm_mutex, K_FOREVER);

			if (pdm_db->available() == 0) {
#ifdef PDM_DEBUG_ENABLED
				if (size > pdm_db->availableForWrite()) {
					Serial.println("[WRG]: mic data loss!");
				}
#endif
				memcpy(pdm_db->data(), buffer, pdm_db->availableForWrite());
				pdm_db->swap(pdm_db->availableForWrite());

				k_mutex_unlock(&pdm_mutex);
				/* notify the user data are ready */
				if (_onReceive) {
					_onReceive();
				}
			} else {
				k_mutex_unlock(&pdm_mutex);
			}
		}
		/* always free slab for next round */
		k_mem_slab_free(&pdm_slab, buffer);
	}
}

/* ---- PDM CLASS ---- */

/* ______________________________________________________________constructor */
PDMClass::PDMClass() : pdm_init(false), active(false) {
}

/* _______________________________________________________________destructor */
PDMClass::~PDMClass() {
}

/* __________________________________________________________________begin() */
int PDMClass::begin(int channels, int sampleRate) {

	/* +++++++++ INITIALISATIONs  +++++++++++++ */

	/* --- SLAB and MUTEX INITIALIZATION (to be performed once) --- */
	if (!pdm_init) {
		int err = k_mem_slab_init(&pdm_slab, pdm_slab_buffer, SLAB_BLOCK_SIZE, SLAB_BLOCK_NUM);
		if (err != 0) {
#ifdef PDM_DEBUG_ENABLED
			Serial.println("[ERR]: slab mic initialization failed with code: " + String(err));
#endif
			return 0; /* failed slab initialization */
		}
		err = k_mutex_init(&pdm_mutex);
		if (err != 0) {
#ifdef PDM_DEBUG_ENABLED
			Serial.println("[ERR]: mutex mic initialization failed with code: " + String(err));
#endif
			return 0; /* failed mutex initialization */
		}
		pdm_init = true;
	}
	/* assing the pointer used by the thread to the "internal" double buffer*/
	pdm_db = &db;

	/* +++++++++++ Start MIC listening +++++++++++++++ */

	if (!active) {
		/* --- give microphone power --- */
#ifdef MIC_PWR_PRESENT
		if (device_is_ready(mic_regulator)) {
#ifdef PDM_DEBUG_ENABLED
			Serial.println("[LOG]: mic regulator enabled");
#endif
			regulator_enable(mic_regulator);
			/* give little time regulator */
			k_msleep(15);
		} else {
			/* [TODO]: suppose the microphone is always powered */
		}
#endif
		/* configure the microphone */
		if (arduino::pdm_configure(channels, sampleRate) < 0) {
			return 0;
		}
		/* --- start the microphone --- */
#ifdef PDM_DEBUG_ENABLED
		Serial.println("[LOG]: mic start listening");
#endif
		if (arduino::pdm_start() < 0) {
#ifdef PDM_DEBUG_ENABLED
			Serial.println("[ERR]: mic START trigger failed");
#endif
			return 0;
		}
		/* --- resume receiving thread --- */
#ifdef PDM_DEBUG_ENABLED
		Serial.println("[LOG]: mic rx thread starting");
#endif
		k_thread_resume(pdm_tid);

		/* --- Set the status as ACTIVE --- */
		active = true;
	}
	return 1;
}

void PDMClass::end() {
	if (active) {
		/* --- stop the microphone --- */
#ifdef PDM_DEBUG_ENABLED
		Serial.println("[LOG]: mic stop listening");
#endif
		if (arduino::pdm_stop() < 0) {
#ifdef PDM_DEBUG_ENABLED
			Serial.println("[ERR]: mic STOP trigger failed");
#endif
		}
		/* --- stop receiving thread --- */
#ifdef PDM_DEBUG_ENABLED
		Serial.println("[LOG]: mic rx thread suspended");
#endif
		k_thread_suspend(pdm_tid);

		/* --- shut down mic regulator --- */
#ifdef MIC_PWR_PRESENT
		regulator_disable(mic_regulator);
#endif

		/* --- Set the status as INACTIVE */
		active = false;
	}
}

/* ______________________________________________________________available() */
int PDMClass::available() {
	k_mutex_lock(&pdm_mutex, K_FOREVER);
	size_t avail = db.available();
	k_mutex_unlock(&pdm_mutex);
	return (int)avail;
}

/* ___________________________________________________________________read() */
int PDMClass::read(void *buffer, size_t size) {
	k_mutex_lock(&pdm_mutex, K_FOREVER);
	int read = db.read(buffer, size);
	k_mutex_unlock(&pdm_mutex);
	return read;
}

/* ______________________________________________________________onReceive() */
void PDMClass::onReceive(void (*func)(void)) {
	_onReceive = func;
}

/* ________________________________________________________________setGain() */
void PDMClass::setGain(int gain) {
	arduino::pdm_gain(gain);
}

/* __________________________________________________________setBufferSize() */
size_t PDMClass::getBufferSize() {
	return db.getSize();
}

PDMClass PDM;
