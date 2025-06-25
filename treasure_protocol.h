#ifndef TREASURE_PROTOCOL_H
#define TREASURE_PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdbool.h>

// Constantes do protocolo
#define MARCADOR 0x7E          // Marcador de início do pacote (01111110)
#define TAM_MAX_DADOS 127      // Tamanho máximo de dados em um pacote
#define TAM_MAX_PACOTE 1500    // Tamanho máximo de um pacote Ethernet
#define ETH_CUSTOM_TYPE 0x88B5 // Tipo Ethernet personalizado para nosso protocolo
#define TAM_MAX_NOME 63        // Tamanho máximo para o nome do arquivo

// Estrutura do grid de jogo
#define GRID_SIZE 8           // Grid 8x8
#define NUM_TESOUROS 8        // Número de tesouros no jogo

// Constantes para timeout e retransmissão
#define TIMEOUT_MS 500        // Timeout em milissegundos
#define MAX_RETRIES 5         // Número máximo de retentativas

// Tipos de mensagens
#define TIPO_ACK 0            // Confirmação
#define TIPO_NACK 1           // Negação
#define TIPO_OK_ACK 2         // OK + confirmação
#define TIPO_TAMANHO 4        // Informa tamanho do arquivo
#define TIPO_DADOS 5          // Dados do arquivo
#define TIPO_TEXTO 6          // Arquivo de texto + ack + nome
#define TIPO_VIDEO 7          // Arquivo de vídeo + ack + nome
#define TIPO_IMAGEM 8         // Arquivo de imagem + ack + nome
#define TIPO_FIM_ARQUIVO 9    // Indica fim de arquivo
#define TIPO_MOVE_DIR 10      // Movimento para direita
#define TIPO_MOVE_CIMA 11     // Movimento para cima
#define TIPO_MOVE_BAIXO 12    // Movimento para baixo
#define TIPO_MOVE_ESQ 13      // Movimento para esquerda
#define TIPO_ERRO 15          // Erro

// Códigos de erro
#define ERRO_SEM_PERMISSAO 0  // Sem permissão de acesso
#define ERRO_ESPACO_INSUF 1   // Espaço insuficiente

// Estados do protocolo para uso no cliente e servidor
typedef enum {
    ESPERANDO_COMANDO,        // Esperando comando do usuário
    ENVIANDO_ARQUIVO,         // Enviando arquivo
    RECEBENDO_ARQUIVO,        // Recebendo arquivo
    PROCESSANDO_MOVIMENTO     // Processando movimento do jogador
} EstadoProtocolo;

// Estrutura para representar uma posição no grid
typedef struct {
    int x;
    int y;
} Posicao;

// Estrutura para representar um tesouro
typedef struct {
    Posicao pos;              // Posição do tesouro no grid
    char nome[TAM_MAX_NOME];  // Nome do arquivo do tesouro
    bool encontrado;          // Indica se o tesouro foi encontrado
} Tesouro;

// Estrutura para representar o estado do jogo
typedef struct {
    Posicao jogador;          // Posição atual do jogador
    Tesouro tesouros[NUM_TESOUROS]; // Lista de tesouros
    bool grid_visitado[GRID_SIZE][GRID_SIZE]; // Grid de posições visitadas
    bool grid_tesouro[GRID_SIZE][GRID_SIZE];  // Grid de posições com tesouros
} EstadoJogo;

// Funções de utilidade para o protocolo
void print_buffer(const char* prefix, unsigned char* buffer, int size);
unsigned char calcula_checksum(unsigned char* dados, int tamanho);
int cria_raw_socket(char* interface);
bool enviar_pacote(int sockfd, struct sockaddr_ll *endereco, unsigned char *mac_destino, 
                  unsigned char *mac_origem, unsigned char tipo, unsigned char seq, 
                  unsigned char *dados, int tam_dados);
bool receber_pacote(int sockfd, unsigned char *buffer, unsigned char *tipo, 
                  unsigned char *seq, unsigned char **dados, int *tam_dados);
bool verifica_espaco_disponivel(const char* diretorio, size_t tamanho_necessario);
int obter_tipo_arquivo(const char *nome_arquivo);
void inicializar_jogo(EstadoJogo *jogo);
bool mover_jogador(EstadoJogo *jogo, int direcao);
int verificar_tesouro(EstadoJogo *jogo);

// Valores para tipo de arquivo
#define TIPO_ARQ_TEXTO 1
#define TIPO_ARQ_VIDEO 2
#define TIPO_ARQ_IMAGEM 3
#define TIPO_ARQ_DESCONHECIDO 0

#endif // TREASURE_PROTOCOL_H 