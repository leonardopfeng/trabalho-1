CC = gcc
CFLAGS = -Wall -g -std=c99 -D_GNU_SOURCE
LIBS = -lpthread

# Arquivos fonte
COMMON_SRC = treasure_protocol.c
SERVER_SRC = treasure_server.c
CLIENT_SRC = treasure_client.c

# Alvos principais
all: server client

# Compilar o servidor
server: $(SERVER_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -o treasure_server $(SERVER_SRC) $(COMMON_SRC) $(LIBS)

# Compilar o cliente
client: $(CLIENT_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -o treasure_client $(CLIENT_SRC) $(COMMON_SRC) $(LIBS)

# Limpar arquivos compilados
clean:
	rm -f treasure_server treasure_client *.o

# Criar diretórios necessários
setup:
	mkdir -p objetos
	mkdir -p recebidos

# Regra para execução do servidor
run-server: server
	sudo ./treasure_server

# Regra para execução do cliente
run-client: client
	sudo ./treasure_client

.PHONY: all clean setup run-server run-client 