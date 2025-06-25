#include "treasure_protocol.h"
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>

// Configuração de rede
#define INTERFACE_NAME "veth0"  // Nome da interface para uso com o virtual Ethernet
#define DIRETORIO_TESOUROS "objetos"  // Diretório onde estão os tesouros

// MAC addresses (podem ser alterados conforme necessário)
static unsigned char mac_cliente[6] = {0xAA, 0xef, 0x89, 0x44, 0x14, 0xd2};
static unsigned char mac_servidor[6] = {0x62, 0x42, 0x03, 0x53, 0xa4, 0x24};

// Variáveis globais
static int sockfd;
static struct sockaddr_ll endereco_cliente;
static EstadoJogo jogo;
static unsigned char ultimo_seq_recebido = 0;
static unsigned char proximo_seq_envio = 0;
static bool em_execucao = true;
static pthread_mutex_t mutex_jogo = PTHREAD_MUTEX_INITIALIZER;
static bool atualizacao_pendente = true; // Nova variável para controlar atualizações

// Funções do servidor
void imprimir_grid();
void *thread_recebimento(void *arg);
bool processar_movimento(unsigned char tipo, unsigned char seq);
bool enviar_arquivo_tesouro(int indice_tesouro);
void inicializar_servidor();
void finalizar_servidor();
void carregar_tipos_tesouros();
void tratar_sinal(int signum);

int main(int argc, char **argv) {
    printf("Iniciando servidor de caça ao tesouro...\n");
    
    // Configurar tratamento de sinais para encerramento limpo
    signal(SIGINT, tratar_sinal);
    signal(SIGTERM, tratar_sinal);
    
    // Inicializar o servidor
    inicializar_servidor();
    
    // Criar diretório de objetos se não existir
    struct stat st = {0};
    if (stat(DIRETORIO_TESOUROS, &st) == -1) {
        mkdir(DIRETORIO_TESOUROS, 0700);
        printf("Diretório %s criado.\n", DIRETORIO_TESOUROS);
    }
    
    // Inicializar o jogo primeiro
    inicializar_jogo(&jogo);
    
    // Depois completar os nomes dos tesouros com as extensões corretas
    // Isso garantirá que os nomes com extensão não sejam sobrescritos
    carregar_tipos_tesouros();
    
    // Exibir a posição dos tesouros
    printf("\nPosições dos tesouros:\n");
    for (int i = 0; i < NUM_TESOUROS; i++) {
        printf("Tesouro %d: (%d,%d) - Arquivo: %s\n", 
               i + 1, jogo.tesouros[i].pos.x, jogo.tesouros[i].pos.y,
               jogo.tesouros[i].nome);
    }
    
    // Exibir o grid inicial
    printf("\nGrid inicial:\n");
    imprimir_grid();
    atualizacao_pendente = false; // Grid inicial já foi impresso
    
    // Criar thread para receber pacotes
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, thread_recebimento, NULL) != 0) {
        perror("Falha ao criar thread de recebimento");
        finalizar_servidor();
        return 1;
    }
    
    // Loop principal - modificado para atualizar apenas quando necessário
    while (em_execucao) {
        // Verifica se há necessidade de atualizar a tela
        pthread_mutex_lock(&mutex_jogo);
        if (atualizacao_pendente) {
            imprimir_grid();
            atualizacao_pendente = false;
        }
        pthread_mutex_unlock(&mutex_jogo);
        
        // Aguarda um pouco antes de verificar novamente
        usleep(500000); // 500ms
    }
    
    // Aguardar a thread terminar
    pthread_join(thread_id, NULL);
    
    // Finalizar o servidor
    finalizar_servidor();
    
    return 0;
}

// Inicializa o servidor
void inicializar_servidor() {
    // Criar o socket raw
    sockfd = cria_raw_socket(INTERFACE_NAME);
    
    // Configurar o endereço do cliente
    memset(&endereco_cliente, 0, sizeof(endereco_cliente));
    endereco_cliente.sll_family = AF_PACKET;
    endereco_cliente.sll_ifindex = if_nametoindex(INTERFACE_NAME);
    endereco_cliente.sll_halen = ETH_ALEN;
    memcpy(endereco_cliente.sll_addr, mac_cliente, 6);
    
    printf("Servidor inicializado. Usando interface %s.\n", INTERFACE_NAME);
}

// Finaliza o servidor
void finalizar_servidor() {
    if (sockfd > 0) {
        close(sockfd);
    }
    pthread_mutex_destroy(&mutex_jogo);
    printf("Servidor finalizado.\n");
}

// Carrega e completa os nomes dos arquivos de tesouro
void carregar_tipos_tesouros() {
    const char *extensoes[] = {".txt", ".jpg", ".mp4"};
    
    // Primeiro, procurar pelos arquivos existentes para cada tesouro
    for (int i = 0; i < NUM_TESOUROS; i++) {
        // Obter o número do tesouro (1-8)
        int num_tesouro = i + 1;
        bool arquivo_encontrado = false;
        
        // Testar cada extensão para este número de tesouro
        for (int j = 0; j < 3; j++) {
            char nome_arquivo[TAM_MAX_NOME];
            snprintf(nome_arquivo, sizeof(nome_arquivo), "%d%s", num_tesouro, extensoes[j]);
            
            char caminho[256];
            snprintf(caminho, sizeof(caminho), "%s/%s", DIRETORIO_TESOUROS, nome_arquivo);
            
            // Verificar se o arquivo existe
            if (access(caminho, F_OK) != -1) {
                // Encontramos um arquivo para este tesouro
                strncpy(jogo.tesouros[i].nome, nome_arquivo, TAM_MAX_NOME);
                arquivo_encontrado = true;
                printf("Arquivo %s associado ao tesouro %d\n", nome_arquivo, num_tesouro);
                break;
            }
        }
        
        if (!arquivo_encontrado) {
            printf("AVISO: Arquivo para o tesouro %d não encontrado!\n", num_tesouro);
            // Mesmo sem encontrar o arquivo, garantimos que o nome tenha uma extensão
            // para evitar erros ao tentar abrir o arquivo
            snprintf(jogo.tesouros[i].nome, TAM_MAX_NOME, "%d%s", num_tesouro, extensoes[0]); // .txt por padrão
            printf("Definindo nome padrão: %s para o tesouro %d\n", jogo.tesouros[i].nome, num_tesouro);
        }
    }
    
    // Verificar quais tesouros têm arquivos na pasta
    printf("\nVerificando arquivos disponíveis em %s:\n", DIRETORIO_TESOUROS);
    char comando[300];
    snprintf(comando, sizeof(comando), "ls -la %s", DIRETORIO_TESOUROS);
    system(comando);
}

// Imprime o grid do jogo
void imprimir_grid() {
    printf("\033[2J\033[H"); // Limpa a tela e posiciona cursor no início
    
    printf("SERVIDOR DE CAÇA AO TESOURO\n");
    printf("===========================\n\n");
    
    // Imprime a posição do jogador
    printf("Posição do jogador: (%d,%d)\n\n", jogo.jogador.x, jogo.jogador.y);
    
    // Imprime tesouros encontrados
    int tesouros_encontrados = 0;
    for (int i = 0; i < NUM_TESOUROS; i++) {
        if (jogo.tesouros[i].encontrado) {
            tesouros_encontrados++;
        }
    }
    printf("Tesouros encontrados: %d de %d\n\n", tesouros_encontrados, NUM_TESOUROS);
    
    // Imprime o grid
    printf("Grid do jogo:\n");
    
    // Imprime números das colunas
    printf("  ");
    for (int x = 0; x < GRID_SIZE; x++) {
        printf(" %d ", x);
    }
    printf("\n");
    
    // Imprime linha separadora
    printf("  ");
    for (int x = 0; x < GRID_SIZE; x++) {
        printf("---");
    }
    printf("\n");
    
    // Imprime o grid linha por linha (de cima para baixo)
    for (int y = GRID_SIZE - 1; y >= 0; y--) {
        printf("%d |", y);
        
        for (int x = 0; x < GRID_SIZE; x++) {
            char celula = ' ';
            
            // Células especiais
            if (x == jogo.jogador.x && y == jogo.jogador.y) {
                celula = 'J'; // Jogador
            } else if (jogo.grid_visitado[y][x]) {
                if (jogo.grid_tesouro[y][x]) {
                    // Verificar se o tesouro já foi encontrado
                    for (int i = 0; i < NUM_TESOUROS; i++) {
                        if (jogo.tesouros[i].pos.x == x && jogo.tesouros[i].pos.y == y && jogo.tesouros[i].encontrado) {
                            celula = 'X'; // Tesouro encontrado
                            break;
                        }
                    }
                    if (celula == ' ') {
                        celula = 'T'; // Tesouro não encontrado
                    }
                } else {
                    celula = '.'; // Célula visitada
                }
            } else if (jogo.grid_tesouro[y][x]) {
                celula = 'T'; // Tesouro (visível apenas no servidor)
            }
            
            printf(" %c ", celula);
        }
        
        printf("|\n");
    }
    
    // Imprime linha separadora
    printf("  ");
    for (int x = 0; x < GRID_SIZE; x++) {
        printf("---");
    }
    printf("\n\n");
    
    // Imprime legenda
    printf("Legenda: J = Jogador, T = Tesouro, X = Tesouro encontrado, . = Visitado\n\n");
    
    // Imprime detalhes dos tesouros
    printf("Detalhes dos tesouros:\n");
    for (int i = 0; i < NUM_TESOUROS; i++) {
        printf("Tesouro %d: (%d,%d) - %s - %s\n", 
               i + 1, 
               jogo.tesouros[i].pos.x, 
               jogo.tesouros[i].pos.y,
               jogo.tesouros[i].nome,
               jogo.tesouros[i].encontrado ? "ENCONTRADO" : "não encontrado");
    }
}

// Thread para receber pacotes do cliente
void *thread_recebimento(void *arg) {
    unsigned char buffer[TAM_MAX_PACOTE];
    unsigned char tipo, seq;
    unsigned char *dados;
    int tam_dados;
    
    printf("Thread de recebimento iniciada.\n");
    
    while (em_execucao) {
        // Tenta receber um pacote
        if (receber_pacote(sockfd, buffer, &tipo, &seq, &dados, &tam_dados)) {
            // Pacote válido recebido
            printf("Pacote recebido: tipo=%d, seq=%d, tam_dados=%d\n", tipo, seq, tam_dados);
            
            // Processar o pacote com base no tipo
            switch (tipo) {
                case TIPO_MOVE_DIR:
                case TIPO_MOVE_ESQ:
                case TIPO_MOVE_CIMA:
                case TIPO_MOVE_BAIXO:
                    pthread_mutex_lock(&mutex_jogo);
                    if (processar_movimento(tipo, seq)) {
                        ultimo_seq_recebido = seq;
                        // Não precisa marcar atualização_pendente aqui,
                        // pois já é feito dentro de processar_movimento
                    }
                    pthread_mutex_unlock(&mutex_jogo);
                    break;
                    
                case TIPO_ACK:
                case TIPO_NACK:
                    // Processamento de ACKs/NACKs para transferência de arquivos
                    break;
                
                default:
                    printf("Tipo de pacote não reconhecido: %d\n", tipo);
                    // Marcar para atualizar a tela mostrando o pacote não reconhecido
                    pthread_mutex_lock(&mutex_jogo);
                    atualizacao_pendente = true;
                    pthread_mutex_unlock(&mutex_jogo);
                    break;
            }
        }
        
        // Pequena pausa para não sobrecarregar o processador
        usleep(10000); // 10ms
    }
    
    printf("Thread de recebimento finalizada.\n");
    return NULL;
}

// Processa um comando de movimento do cliente
bool processar_movimento(unsigned char tipo, unsigned char seq) {
    printf("Processando movimento: tipo=%d\n", tipo);
    
    // Enviar ACK para o cliente
    enviar_pacote(sockfd, &endereco_cliente, mac_cliente, mac_servidor, 
                 TIPO_ACK, seq, NULL, 0);
    
    // Atualizar posição do jogador
    if (!mover_jogador(&jogo, tipo)) {
        printf("Movimento inválido! Fora dos limites do grid.\n");
        return false;
    }
    
    printf("Jogador moveu para (%d,%d)\n", jogo.jogador.x, jogo.jogador.y);
    
    // Marcar que uma atualização da tela é necessária
    atualizacao_pendente = true;
    
    // Verificar se há tesouro na nova posição
    int indice_tesouro = verificar_tesouro(&jogo);
    if (indice_tesouro > 0) {
        printf("Tesouro %d encontrado na posição (%d,%d)!\n", 
               indice_tesouro, jogo.jogador.x, jogo.jogador.y);
        printf("Nome do arquivo do tesouro: '%s'\n", jogo.tesouros[indice_tesouro-1].nome);
        
        // Enviar o arquivo do tesouro para o cliente
        if (enviar_arquivo_tesouro(indice_tesouro - 1)) {
            printf("Arquivo do tesouro enviado com sucesso.\n");
        } else {
            printf("Falha ao enviar arquivo do tesouro.\n");
        }
    }
    
    return true;
}

// Envia um arquivo de tesouro para o cliente
bool enviar_arquivo_tesouro(int indice_tesouro) {
    if (indice_tesouro < 0 || indice_tesouro >= NUM_TESOUROS) {
        printf("Índice de tesouro inválido: %d\n", indice_tesouro);
        return false;
    }
    
    Tesouro *tesouro = &jogo.tesouros[indice_tesouro];
    printf("Enviando tesouro %d: nome='%s', posição=(%d,%d)\n", 
           indice_tesouro + 1, tesouro->nome, tesouro->pos.x, tesouro->pos.y);
    
    // Caminho completo do arquivo
    char caminho[256];
    snprintf(caminho, sizeof(caminho), "%s/%s", DIRETORIO_TESOUROS, tesouro->nome);
    printf("Tentando abrir arquivo: '%s'\n", caminho);
    
    // Abrir o arquivo
    FILE *arquivo = fopen(caminho, "rb");
    
    // Se não encontramos o arquivo, tentamos adicionar uma extensão .txt
    if (!arquivo) {
        perror("Erro ao abrir arquivo de tesouro");
        
        // Verificar se o nome já tem extensão
        const char *ponto = strchr(tesouro->nome, '.');
        if (ponto == NULL) {
            // Se não tem extensão, tente adicionar .txt
            // Limitar o tamanho do nome original para evitar truncamento ao adicionar a extensão
            char nome_base[TAM_MAX_NOME - 5]; // Reservar espaço para ".txt" e null terminator
            strncpy(nome_base, tesouro->nome, sizeof(nome_base) - 1);
            nome_base[sizeof(nome_base) - 1] = '\0'; // Garantir terminação
            
            char novo_nome[TAM_MAX_NOME];
            snprintf(novo_nome, sizeof(novo_nome), "%s.txt", nome_base);
            
            // Atualiza o caminho com a extensão
            snprintf(caminho, sizeof(caminho), "%s/%s", DIRETORIO_TESOUROS, novo_nome);
            printf("Tentando novamente com extensão .txt: '%s'\n", caminho);
            
            arquivo = fopen(caminho, "rb");
            
            // Se encontramos o arquivo com a extensão, atualizamos o nome do tesouro
            if (arquivo) {
                strncpy(tesouro->nome, novo_nome, TAM_MAX_NOME);
                printf("Arquivo encontrado com extensão. Atualizando nome do tesouro para: %s\n", 
                      tesouro->nome);
            }
        }
        
        if (!arquivo) {
            // Tentar encontrar e listar os arquivos disponíveis no diretório
            printf("Listando arquivos em %s:\n", DIRETORIO_TESOUROS);
            char comando[300];
            snprintf(comando, sizeof(comando), "ls -la %s", DIRETORIO_TESOUROS);
            system(comando);
            
            // Tentar encontrar um arquivo que comece com o mesmo número
            printf("Procurando por qualquer arquivo que comece com '%d'...\n", indice_tesouro + 1);
            
            // Verificar arquivos com diferentes extensões
            const char *extensoes[] = {".txt", ".jpg", ".mp4"};
            for (int j = 0; j < 3; j++) {
                char nome_possivel[TAM_MAX_NOME];
                snprintf(nome_possivel, sizeof(nome_possivel), "%d%s", indice_tesouro + 1, extensoes[j]);
                
                snprintf(caminho, sizeof(caminho), "%s/%s", DIRETORIO_TESOUROS, nome_possivel);
                printf("Verificando: %s\n", caminho);
                
                arquivo = fopen(caminho, "rb");
                if (arquivo) {
                    // Se encontramos o arquivo, atualizamos o nome do tesouro
                    strncpy(tesouro->nome, nome_possivel, TAM_MAX_NOME);
                    printf("Arquivo alternativo encontrado. Atualizando nome do tesouro para: %s\n", 
                          tesouro->nome);
                    break;
                }
            }
            
            if (!arquivo) {
                return false;
            }
        }
    }
    
    // Obter tamanho do arquivo
    struct stat st;
    if (stat(caminho, &st) == -1) {
        perror("Erro ao obter tamanho do arquivo");
        fclose(arquivo);
        return false;
    }
    size_t tamanho_arquivo = st.st_size;
    
    // Determinar o tipo de mensagem com base na extensão
    unsigned char tipo_mensagem;
    int tipo_arquivo = obter_tipo_arquivo(tesouro->nome);
    
    switch (tipo_arquivo) {
        case TIPO_ARQ_TEXTO:
            tipo_mensagem = TIPO_TEXTO;
            break;
        case TIPO_ARQ_VIDEO:
            tipo_mensagem = TIPO_VIDEO;
            break;
        case TIPO_ARQ_IMAGEM:
            tipo_mensagem = TIPO_IMAGEM;
            break;
        default:
            tipo_mensagem = TIPO_TEXTO; // Padrão para arquivos desconhecidos
            break;
    }
    
    // Enviar informação de tamanho
    unsigned char dados_tamanho[sizeof(size_t)];
    memcpy(dados_tamanho, &tamanho_arquivo, sizeof(size_t));
    
    if (!enviar_pacote(sockfd, &endereco_cliente, mac_cliente, mac_servidor, 
                      TIPO_TAMANHO, proximo_seq_envio, dados_tamanho, sizeof(size_t))) {
        fclose(arquivo);
        return false;
    }
    
    // Aguardar ACK pelo tamanho
    unsigned char buffer[TAM_MAX_PACOTE];
    unsigned char tipo_resp, seq_resp;
    unsigned char *dados_resp;
    int tam_dados_resp;
    bool ack_recebido = false;
    
    // Loop com timeout para esperar ACK
    struct timeval inicio, atual;
    gettimeofday(&inicio, NULL);
    
    while (!ack_recebido) {
        // Verificar timeout
        gettimeofday(&atual, NULL);
        long elapsed = (atual.tv_sec - inicio.tv_sec) * 1000 + 
                      (atual.tv_usec - inicio.tv_usec) / 1000;
        
        if (elapsed > TIMEOUT_MS) {
            printf("Timeout esperando ACK para tamanho do arquivo.\n");
            fclose(arquivo);
            return false;
        }
        
        if (receber_pacote(sockfd, buffer, &tipo_resp, &seq_resp, &dados_resp, &tam_dados_resp)) {
            if (tipo_resp == TIPO_ACK && seq_resp == proximo_seq_envio) {
                ack_recebido = true;
                proximo_seq_envio = (proximo_seq_envio + 1) % 32;
            }
        }
        
        usleep(10000); // 10ms
    }
    
    // Enviar nome do arquivo
    size_t tam_nome = strlen(tesouro->nome) + 1;
    if (!enviar_pacote(sockfd, &endereco_cliente, mac_cliente, mac_servidor, 
                      tipo_mensagem, proximo_seq_envio, (unsigned char*)tesouro->nome, tam_nome)) {
        fclose(arquivo);
        return false;
    }
    
    // Aguardar ACK pelo nome
    ack_recebido = false;
    gettimeofday(&inicio, NULL);
    
    while (!ack_recebido) {
        // Verificar timeout
        gettimeofday(&atual, NULL);
        long elapsed = (atual.tv_sec - inicio.tv_sec) * 1000 + 
                      (atual.tv_usec - inicio.tv_usec) / 1000;
        
        if (elapsed > TIMEOUT_MS) {
            printf("Timeout esperando ACK para nome do arquivo.\n");
            fclose(arquivo);
            return false;
        }
        
        if (receber_pacote(sockfd, buffer, &tipo_resp, &seq_resp, &dados_resp, &tam_dados_resp)) {
            if (tipo_resp == TIPO_ACK && seq_resp == proximo_seq_envio) {
                ack_recebido = true;
                proximo_seq_envio = (proximo_seq_envio + 1) % 32;
            }
        }
        
        usleep(10000); // 10ms
    }
    
    // Enviar dados do arquivo em chunks
    unsigned char dados_arquivo[TAM_MAX_DADOS];
    size_t bytes_lidos;
    int retries;
    
    while ((bytes_lidos = fread(dados_arquivo, 1, TAM_MAX_DADOS, arquivo)) > 0) {
        retries = 0;
        ack_recebido = false;
        
        while (!ack_recebido && retries < MAX_RETRIES) {
            // Enviar chunk de dados
            if (!enviar_pacote(sockfd, &endereco_cliente, mac_cliente, mac_servidor, 
                             TIPO_DADOS, proximo_seq_envio, dados_arquivo, bytes_lidos)) {
                printf("Erro ao enviar dados do arquivo.\n");
                retries++;
                continue;
            }
            
            // Aguardar ACK
            gettimeofday(&inicio, NULL);
            bool timeout = false;
            
            while (!ack_recebido && !timeout) {
                // Verificar timeout
                gettimeofday(&atual, NULL);
                long elapsed = (atual.tv_sec - inicio.tv_sec) * 1000 + 
                               (atual.tv_usec - inicio.tv_usec) / 1000;
                
                if (elapsed > TIMEOUT_MS) {
                    printf("Timeout esperando ACK para dados. Tentativa %d/%d.\n", 
                          retries + 1, MAX_RETRIES);
                    timeout = true;
                    retries++;
                    continue;
                }
                
                if (receber_pacote(sockfd, buffer, &tipo_resp, &seq_resp, &dados_resp, &tam_dados_resp)) {
                    if (tipo_resp == TIPO_ACK && seq_resp == proximo_seq_envio) {
                        ack_recebido = true;
                        proximo_seq_envio = (proximo_seq_envio + 1) % 32;
                    } else if (tipo_resp == TIPO_NACK && seq_resp == proximo_seq_envio) {
                        printf("NACK recebido. Retransmitindo...\n");
                        timeout = true;
                        retries++;
                    }
                }
                
                usleep(10000); // 10ms
            }
        }
        
        if (retries >= MAX_RETRIES) {
            printf("Número máximo de tentativas excedido.\n");
            fclose(arquivo);
            return false;
        }
    }
    
    // Enviar mensagem de fim de arquivo
    if (!enviar_pacote(sockfd, &endereco_cliente, mac_cliente, mac_servidor, 
                     TIPO_FIM_ARQUIVO, proximo_seq_envio, NULL, 0)) {
        fclose(arquivo);
        return false;
    }
    
    // Aguardar ACK final
    ack_recebido = false;
    gettimeofday(&inicio, NULL);
    
    while (!ack_recebido) {
        // Verificar timeout
        gettimeofday(&atual, NULL);
        long elapsed = (atual.tv_sec - inicio.tv_sec) * 1000 + 
                      (atual.tv_usec - inicio.tv_usec) / 1000;
        
        if (elapsed > TIMEOUT_MS) {
            printf("Timeout esperando ACK final.\n");
            fclose(arquivo);
            return false;
        }
        
        if (receber_pacote(sockfd, buffer, &tipo_resp, &seq_resp, &dados_resp, &tam_dados_resp)) {
            if (tipo_resp == TIPO_ACK && seq_resp == proximo_seq_envio) {
                ack_recebido = true;
                proximo_seq_envio = (proximo_seq_envio + 1) % 32;
            }
        }
        
        usleep(10000); // 10ms
    }
    
    fclose(arquivo);
    
    // Marcar que o grid precisa ser atualizado para mostrar o tesouro encontrado
    atualizacao_pendente = true;
    
    return true;
}

// Tratamento de sinais para encerramento limpo
void tratar_sinal(int signum) {
    printf("\nSinal %d recebido. Encerrando servidor...\n", signum);
    em_execucao = false;
} 