#include "device_health.h"

void device_health_reset(struct device_health *h)
{
	*h = (struct device_health){0};
}

/* Fecha a janela de transacoes quando ela enche, congelando failure_rate -
 * mesma logica em comScanSuccess() e comScanDevFail() do original (os
 * dois fazem exatamente isto, so' que duplicado em cada funcao).
 */
static void bump_window(struct device_health *h)
{
	if (h->count_transaction < DEVICE_HEALTH_RATE_WINDOW) {
		h->count_transaction++;
	} else {
		h->failure_rate = h->count_failure;
		h->count_failure = 0;
		h->count_transaction = 0;
	}
}

void device_health_record_success(struct device_health *h)
{
	h->ever_succeeded = true;
	h->count_failure_sequent = 0;
	bump_window(h);
}

void device_health_record_failure(struct device_health *h)
{
	if (h->count_failure_sequent < DEVICE_HEALTH_MAX_FAILURE_SEQUENT) {
		h->count_failure_sequent++;
	}
	if (h->count_failure < DEVICE_HEALTH_RATE_WINDOW) {
		h->count_failure++;
	}
	bump_window(h);
}

bool device_health_is_online(const struct device_health *h)
{
	if (!h->ever_succeeded) {
		return false;
	}
	if (h->count_failure_sequent >= DEVICE_HEALTH_MAX_FAILURE_SEQUENT) {
		return false;
	}
	return true;
}

uint16_t device_health_failure_rate(const struct device_health *h)
{
	return h->failure_rate;
}

uint16_t device_health_count_transaction(const struct device_health *h)
{
	return h->count_transaction;
}

uint16_t device_health_count_failure_sequent(const struct device_health *h)
{
	return h->count_failure_sequent;
}
