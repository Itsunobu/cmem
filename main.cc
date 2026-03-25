#include "c_mem.h"

int main(int ac, char **av)
{
	mcp::Server server("c-mem", "0.1.0");
	cmem::CMem cmem;
	cmem.registerTools(server);
	server.run();
	return 0;
}
