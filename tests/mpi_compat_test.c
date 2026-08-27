// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpi.h>

static int test_exchange(int rank, size_t length)
{
	unsigned char *received;
	unsigned char *sent;
	size_t i;
	int peer = rank ^ 1;
	int failed = 0;
	int rc;

	sent = malloc(length);
	received = malloc(length);
	if (!sent || !received) {
		free(received);
		free(sent);
		return 1;
	}
	for (i = 0; i < length; i++)
		sent[i] = (unsigned char)(rank * 73 + i);
	memset(received, 0, length);
	rc = MPI_Sendrecv(sent, (int)length, MPI_BYTE, peer, 1,
			  received, (int)length, MPI_BYTE, peer, 1,
			  MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	if (rc != MPI_SUCCESS)
		failed = 1;
	for (i = 0; !failed && i < length; i++) {
		if (received[i] != (unsigned char)(peer * 73 + i))
			failed = 1;
	}
	free(received);
	free(sent);
	return failed;
}

int main(int argc, char **argv)
{
	static const size_t lengths[] = {1, 2051, 65536};
	int local_fail = 0;
	int global_fail;
	int rank;
	int size;
	int sum;
	int value;
	size_t i;

	if (MPI_Init(&argc, &argv) != MPI_SUCCESS)
		return 1;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	if (size != 2) {
		if (!rank)
			fprintf(stderr, "MPI compatibility test requires two ranks\n");
		local_fail = 1;
	}
	for (i = 0; !local_fail && i < sizeof(lengths) / sizeof(lengths[0]); i++)
		local_fail |= test_exchange(rank, lengths[i]);
	value = rank + 1;
	if (MPI_Allreduce(&value, &sum, 1, MPI_INT, MPI_SUM,
			  MPI_COMM_WORLD) != MPI_SUCCESS || sum != 3)
		local_fail = 1;
	MPI_Allreduce(&local_fail, &global_fail, 1, MPI_INT, MPI_MAX,
		      MPI_COMM_WORLD);
	if (!rank)
		printf("MPI communication over UIB: %s\n",
		       global_fail ? "FAIL" : "PASS");
	MPI_Finalize();
	return global_fail ? 1 : 0;
}
