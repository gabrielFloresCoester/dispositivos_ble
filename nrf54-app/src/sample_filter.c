#include "sample_filter.h"

void sample_filter_push(struct sample_filter *filter, int32_t value)
{
	filter->samples[filter->next] = value;
	filter->next = (filter->next + 1) % SAMPLE_FILTER_LEN;
	if (filter->filled < SAMPLE_FILTER_LEN) {
		filter->filled++;
	}
}

void sample_filter_reset(struct sample_filter *filter)
{
	filter->next = 0;
	filter->filled = 0;
}

bool sample_filter_avg(struct sample_filter *filter, int32_t *avg_out)
{
	int32_t max_val, min_val, sum;

	if (filter->filled < SAMPLE_FILTER_LEN) {
		return false;
	}

	sum = 0;
	max_val = filter->samples[0];
	min_val = filter->samples[0];

	for (int i = 0; i < SAMPLE_FILTER_LEN; i++) {
		int32_t v = filter->samples[i];

		sum += v;
		if (v > max_val) {
			max_val = v;
		}
		if (v < min_val) {
			min_val = v;
		}
	}

	sum -= max_val;
	sum -= min_val;

	*avg_out = sum / (SAMPLE_FILTER_LEN - 2);
	return true;
}
