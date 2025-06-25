#include "treasure_protocol.h"
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <ctype.h>

// Configuração de rede
#define INTERFACE_NAME "veth1"  // Nome da interface para uso com o virtual Ethernet
#define DIRETORIO_RECEBIDOS "recebidos"  // Diretório onde serão salvos os arquivos recebidos

// MAC addresses (devem corresponder aos do servidor)
static unsigned char mac_cliente[6] = {0xAA, 0xef, 0x89, 0x44, 0x14, 0xd2};
static unsigned char mac_servidor[6] = {0x62, 0x42, 0x03, 0x53, 0xa4, 0x24};

// Variáveis globais
static int sockfd;
static struct sockaddr_ll endereco_servidor;
static EstadoJogo jogo;
static unsigned char ultimo_seq_recebido = 0;
static unsigned char proximo_seq_envio = 0;
static bool em_execucao = true;
static pthread_mutex_t mutex_jogo = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_recebimento = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_recebimento = PTHREAD_COND_INITIALIZER;
static bool aguardando_arquivo = false;
static char nome_arquivo_recebido[TAM_MAX_NOME];
static int tipo_arquivo_recebido = TIPO_ARQ_DESCONHECIDO;
static FILE *arquivo_recebendo = NULL; // Ponteiro para o arquivo que está sendo recebido

// Novas variáveis para controle de movimentos
static bool movimento_em_andamento = false;
static bool ultimo_movimento_ok = false;
static pthread_mutex_t mutex_movimento = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_movimento = PTHREAD_COND_INITIALIZER;
static unsigned char ultimo_movimento_enviado = 0; // Armazena o tipo do último movimento enviado
static bool atualizacao_pendente = true; // Indica que o grid precisa ser redesenhado

// Funções do cliente
void imprimir_grid();
void *thread_recebimento(void *arg);
bool enviar_movimento(int direcao);
bool iniciar_recebimento_arquivo(const char *nome_arquivo);
void finalizar_recebimento_arquivo(bool sucesso);
void inicializar_cliente();
void finalizar_cliente();
void imprimir_menu();
void tratar_sinal(int signum);

int main(int argc, char **argv) {
    printf("Iniciando cliente de caça ao tesouro...\n");
    
    // Configurar tratamento de sinais para encerramento limpo
    signal(SIGINT, tratar_sinal);
    signal(SIGTERM, tratar_sinal);
    
    // Inicializar o cliente
    inicializar_cliente();
    
    // Criar diretório para arquivos recebidos se não existir
    struct stat st = {0};
    if (stat(DIRETORIO_RECEBIDOS, &st) == -1) {
        // Criar com permissões mais liberais (owner, group e others podem ler/escrever/executar)
        if (mkdir(DIRETORIO_RECEBIDOS, 0777) == -1) {
            perror("Erro ao criar diretório recebidos");
            printf("Tentando criar o diretório com permissões mais amplas...\n");
            
            // Tentar criar o diretório usando comandos do sistema
            char cmd[1024]; // Aumentado para 1024 bytes
            snprintf(cmd, sizeof(cmd), "mkdir -p %s && chmod 777 %s", DIRETORIO_RECEBIDOS, DIRETORIO_RECEBIDOS);
            system(cmd);
            
            // Verificar se foi criado
            if (stat(DIRETORIO_RECEBIDOS, &st) == -1) {
                printf("AVISO: Não foi possível criar o diretório %s. Os arquivos não serão salvos.\n", DIRETORIO_RECEBIDOS);
            } else {
                printf("Diretório %s criado via comando do sistema.\n", DIRETORIO_RECEBIDOS);
            }
        } else {
            printf("Diretório %s criado com permissões 0777.\n", DIRETORIO_RECEBIDOS);
            
            // Garantir que as permissões estão corretas
            chmod(DIRETORIO_RECEBIDOS, 0777);
        }
    } else {
        // Diretório já existe, verificar permissões
        printf("Diretório %s já existe. Verificando permissões...\n", DIRETORIO_RECEBIDOS);
        
        // Garantir que as permissões estão corretas
        if (chmod(DIRETORIO_RECEBIDOS, 0777) == -1) {
            perror("Não foi possível alterar as permissões do diretório");
            // Tentar via sistema
            char cmd[1024]; // Aumentado para 1024 bytes
            snprintf(cmd, sizeof(cmd), "chmod 777 %s", DIRETORIO_RECEBIDOS);
            system(cmd);
        } else {
            printf("Permissões do diretório %s atualizadas para 0777.\n", DIRETORIO_RECEBIDOS);
        }
    }
    
    // Verificar se conseguimos escrever no diretório
    char test_path[512]; // Aumentado para 512 bytes
    snprintf(test_path, sizeof(test_path), "%s/.test_write", DIRETORIO_RECEBIDOS);
    FILE *test_file = fopen(test_path, "w");
    if (test_file) {
        fclose(test_file);
        remove(test_path);
        printf("Teste de escrita no diretório %s bem-sucedido.\n", DIRETORIO_RECEBIDOS);
    } else {
        perror("Teste de escrita no diretório falhou");
        printf("AVISO: Pode haver problemas ao salvar arquivos em %s\n", DIRETORIO_RECEBIDOS);
    }
    
    // Inicializar o jogo (grid vazio)
    inicializar_jogo(&jogo);
    
    // Criar thread para receber pacotes
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, thread_recebimento, NULL) != 0) {
        perror("Falha ao criar thread de recebimento");
        finalizar_cliente();
        return 1;
    }
    
    // Loop principal
    char comando;
    bool entrada_valida;
    
    while (em_execucao) {
        // Verificar se precisamos atualizar o grid
        pthread_mutex_lock(&mutex_jogo);
        if (atualizacao_pendente) {
            imprimir_grid();
            imprimir_menu();
            atualizacao_pendente = false;
        }
        pthread_mutex_unlock(&mutex_jogo);
        
        // Obter comando do usuário se não estivermos aguardando um arquivo
        pthread_mutex_lock(&mutex_recebimento);
        bool pode_obter_comando = !aguardando_arquivo;
        pthread_mutex_unlock(&mutex_recebimento);
        
        // Verificar se há um movimento em andamento
        pthread_mutex_lock(&mutex_movimento);
        pode_obter_comando = pode_obter_comando && !movimento_em_andamento;
        pthread_mutex_unlock(&mutex_movimento);
        
        if (pode_obter_comando) {
            printf("Digite o comando: ");
            fflush(stdout);
            comando = getchar();
            
            // Limpar o buffer de entrada
            while (getchar() != '\n');
            
            // Converter para minúsculo
            comando = tolower(comando);
            
            entrada_valida = false;
            switch (comando) {
                case 'w': // Cima
                    entrada_valida = enviar_movimento(TIPO_MOVE_CIMA);
                    break;
                case 's': // Baixo
                    entrada_valida = enviar_movimento(TIPO_MOVE_BAIXO);
                    break;
                case 'a': // Esquerda
                    entrada_valida = enviar_movimento(TIPO_MOVE_ESQ);
                    break;
                case 'd': // Direita
                    entrada_valida = enviar_movimento(TIPO_MOVE_DIR);
                    break;
                default:
                    printf("Comando inválido!\n");
                    atualizacao_pendente = true; // Forçar atualização para mostrar comando inválido
                    break;
            }
            
            if (!entrada_valida && em_execucao && comando != '\n') {
                printf("Tente novamente.\n");
                atualizacao_pendente = true; // Forçar atualização para mostrar mensagem
            }
        }
        
        // Aguardar um pouco antes de verificar novamente
        usleep(100000); // 100ms
    }
    
    // Aguardar a thread terminar
    pthread_join(thread_id, NULL);
    
    // Finalizar o cliente
    finalizar_cliente();
    
    return 0;
}

// Inicializa o cliente
void inicializar_cliente() {
    // Criar o socket raw
    sockfd = cria_raw_socket(INTERFACE_NAME);
    
    // Configurar o endereço do servidor
    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sll_family = AF_PACKET;
    endereco_servidor.sll_ifindex = if_nametoindex(INTERFACE_NAME);
    endereco_servidor.sll_halen = ETH_ALEN;
    memcpy(endereco_servidor.sll_addr, mac_servidor, 6);
    
    printf("Cliente inicializado. Usando interface %s.\n", INTERFACE_NAME);
}

// Finaliza o cliente
void finalizar_cliente() {
    // Fechar qualquer arquivo aberto
    if (arquivo_recebendo != NULL) {
        fclose(arquivo_recebendo);
        arquivo_recebendo = NULL;
        printf("Arquivo aberto fechado durante finalização.\n");
    }
    
    if (sockfd > 0) {
        close(sockfd);
    }
    pthread_mutex_destroy(&mutex_jogo);
    pthread_mutex_destroy(&mutex_recebimento);
    pthread_cond_destroy(&cond_recebimento);
    pthread_mutex_destroy(&mutex_movimento);
    pthread_cond_destroy(&cond_movimento);
    printf("Cliente finalizado.\n");
}

// Imprime o grid do jogo
void imprimir_grid() {
    printf("\033[2J\033[H"); // Limpa a tela e posiciona cursor no início
    
    printf("CLIENTE - CAÇA AO TESOURO\n");
    printf("=========================\n\n");
    
    // Imprime a posição do jogador
    printf("Sua posição: (%d,%d)\n\n", jogo.jogador.x, jogo.jogador.y);
    
    // Conta tesouros encontrados
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
                bool tesouro_encontrado = false;
                
                // Verificar se nesta posição há um tesouro encontrado
                for (int i = 0; i < NUM_TESOUROS; i++) {
                    if (jogo.tesouros[i].pos.x == x && jogo.tesouros[i].pos.y == y && jogo.tesouros[i].encontrado) {
                        celula = 'X'; // Tesouro encontrado
                        tesouro_encontrado = true;
                        break;
                    }
                }
                
                if (!tesouro_encontrado) {
                    celula = '.'; // Célula visitada sem tesouro
                }
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
    printf("Legenda: J = Jogador, X = Tesouro encontrado, . = Visitado\n");
    
    // Imprime lista de tesouros encontrados
    printf("Tesouros encontrados:\n");
    
    if (tesouros_encontrados > 0) {
        // Tabela formatada e colorida dos tesouros encontrados
        printf("+------+----------------------+-------------+\n");
        printf("| \033[1mNum\033[0m  | \033[1mArquivo              \033[0m | \033[1mPosição     \033[0m |\n");
        printf("+------+----------------------+-------------+\n");
        
        for (int i = 0; i < NUM_TESOUROS; i++) {
            if (jogo.tesouros[i].encontrado) {
                printf("| \033[1;32m%-4d\033[0m | \033[1;33m%-20s\033[0m | (%2d,%2d)     |\n", 
                       i + 1, 
                       jogo.tesouros[i].nome,
                       jogo.tesouros[i].pos.x,
                       jogo.tesouros[i].pos.y);
            }
        }
        
        printf("+------+----------------------+-------------+\n");
    } else {
        printf("\033[3mNenhum tesouro encontrado ainda.\033[0m\n");
    }
    
    printf("\n");
}

// Imprime o menu de comandos
void imprimir_menu() {
    printf("Comandos:\n");
    printf("  W - Mover para cima\n");
    printf("  S - Mover para baixo\n");
    printf("  A - Mover para esquerda\n");
    printf("  D - Mover para direita\n");
}

// Envia um comando de movimento para o servidor
bool enviar_movimento(int direcao) {
    // Validar direção
    if (direcao != TIPO_MOVE_DIR && direcao != TIPO_MOVE_ESQ && 
        direcao != TIPO_MOVE_CIMA && direcao != TIPO_MOVE_BAIXO) {
        return false;
    }
    
    // Verificar se já existe um movimento em andamento
    pthread_mutex_lock(&mutex_movimento);
    if (movimento_em_andamento) {
        printf("Ainda processando o movimento anterior. Aguarde...\n");
        pthread_mutex_unlock(&mutex_movimento);
        return false;
    }
    
    // Marcar que estamos iniciando um movimento
    movimento_em_andamento = true;
    ultimo_movimento_ok = false;
    ultimo_movimento_enviado = direcao; // Armazenar o tipo do movimento que está sendo enviado
    pthread_mutex_unlock(&mutex_movimento);
    
    // Enviar o comando
    if (!enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                      direcao, proximo_seq_envio, NULL, 0)) {
        printf("Erro ao enviar comando de movimento.\n");
        
        // Liberar o bloqueio de movimento
        pthread_mutex_lock(&mutex_movimento);
        movimento_em_andamento = false;
        pthread_mutex_unlock(&mutex_movimento);
        
        return false;
    }
    
    printf("Movimento enviado. Aguardando resposta do servidor...\n");
    
    // Aguardar resposta do servidor (será processada pela thread_recebimento)
    struct timespec timeout;
    struct timeval now;
    gettimeofday(&now, NULL);
    timeout.tv_sec = now.tv_sec + (TIMEOUT_MS / 1000);
    timeout.tv_nsec = (now.tv_usec + (TIMEOUT_MS % 1000) * 1000) * 1000;
    
    // Normalizar os valores de nanosegundos
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec++;
        timeout.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_lock(&mutex_movimento);
    int result = 0;
    
    // Enquanto o movimento estiver em andamento e não tivermos atingido o timeout
    while (movimento_em_andamento && result != ETIMEDOUT && em_execucao) {
        result = pthread_cond_timedwait(&cond_movimento, &mutex_movimento, &timeout);
    }
    
    bool sucesso = ultimo_movimento_ok;
    movimento_em_andamento = false;
    pthread_mutex_unlock(&mutex_movimento);
    
    if (result == ETIMEDOUT) {
        printf("Timeout aguardando resposta do servidor.\n");
        return false;
    }
    
    return sucesso;
}

// Thread para receber pacotes do servidor
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
                case TIPO_ACK:
                    // Verificar se é uma resposta a um comando de movimento
                    pthread_mutex_lock(&mutex_movimento);
                    if (movimento_em_andamento && seq == proximo_seq_envio) {
                        // Simular o movimento localmente
                        pthread_mutex_lock(&mutex_jogo);
                        
                        // Aplicar o último movimento enviado
                        if (mover_jogador(&jogo, ultimo_movimento_enviado)) {
                            printf("Movimento aplicado localmente\n");
                            atualizacao_pendente = true; // Marcar que o grid precisa ser atualizado
                        } else {
                            printf("Erro ao aplicar movimento localmente.\n");
                            atualizacao_pendente = true; // Marcar que o grid precisa ser atualizado
                        }
                        
                        pthread_mutex_unlock(&mutex_jogo);
                        
                        // Atualizar o controle de sequência
                        proximo_seq_envio = (proximo_seq_envio + 1) % 32;
                        
                        // Sinalizar que o movimento foi concluído com sucesso
                        ultimo_movimento_ok = true;
                        movimento_em_andamento = false;
                        pthread_cond_signal(&cond_movimento);
                    }
                    pthread_mutex_unlock(&mutex_movimento);
                    break;
                
                case TIPO_TEXTO:
                case TIPO_VIDEO:
                case TIPO_IMAGEM:
                    // Recebeu início de arquivo com tipo conhecido
                    pthread_mutex_lock(&mutex_recebimento);
                    
                    // Armazenar informações do arquivo
                    if (tam_dados > 0 && dados != NULL) {
                        strncpy(nome_arquivo_recebido, (char *)dados, TAM_MAX_NOME);
                        
                        // Determinar o tipo de arquivo
                        tipo_arquivo_recebido = obter_tipo_arquivo(nome_arquivo_recebido);
                        
                        // Iniciar recebimento do arquivo
                        if (iniciar_recebimento_arquivo(nome_arquivo_recebido)) {
                            // Sinalizar que estamos aguardando um arquivo
                            aguardando_arquivo = true;
                            
                            // Enviar ACK
                            enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                                        TIPO_ACK, seq, NULL, 0);
                            
                            // Atualizar último sequencial recebido
                            ultimo_seq_recebido = seq;
                            
                            printf("Iniciando recebimento do arquivo %s...\n", nome_arquivo_recebido);
                        } else {
                            printf("Falha ao iniciar recebimento do arquivo %s.\n", nome_arquivo_recebido);
                            aguardando_arquivo = false;
                        }
                    }
                    
                    pthread_mutex_unlock(&mutex_recebimento);
                    break;
                    
                case TIPO_DADOS:
                    // Processa dados do arquivo sendo recebido
                    pthread_mutex_lock(&mutex_recebimento);
                    if (aguardando_arquivo && arquivo_recebendo != NULL && tam_dados > 0 && dados != NULL) {
                        // Escrever no arquivo
                        size_t escritos = fwrite(dados, 1, tam_dados, arquivo_recebendo);
                        
                        if (escritos != (size_t)tam_dados) {
                            perror("Erro ao escrever no arquivo");
                            enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                                        TIPO_NACK, seq, NULL, 0);
                            finalizar_recebimento_arquivo(false);
                        } else {
                            // Enviar ACK
                            enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                                        TIPO_ACK, seq, NULL, 0);
                            
                            // Atualizar último sequencial recebido
                            ultimo_seq_recebido = seq;
                        }
                    }
                    pthread_mutex_unlock(&mutex_recebimento);
                    break;
                    
                case TIPO_FIM_ARQUIVO:
                    // Finaliza o recebimento do arquivo
                    pthread_mutex_lock(&mutex_recebimento);
                    if (aguardando_arquivo && arquivo_recebendo != NULL) {
                        // Enviar ACK
                        enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                                    TIPO_ACK, seq, NULL, 0);
                        
                        // Atualizar último sequencial recebido
                        ultimo_seq_recebido = seq;
                        
                        // Finalizar recebimento
                        finalizar_recebimento_arquivo(true);
                        
                        printf("Arquivo %s recebido com sucesso!\n", nome_arquivo_recebido);
                                             
                        // Adicionar o tesouro à lista de tesouros encontrados
                        pthread_mutex_lock(&mutex_jogo);
                        for (int i = 0; i < NUM_TESOUROS; i++) {
                            if (jogo.tesouros[i].encontrado == false && 
                                jogo.jogador.x == jogo.tesouros[i].pos.x && 
                                jogo.jogador.y == jogo.tesouros[i].pos.y) {
                                
                                strncpy(jogo.tesouros[i].nome, nome_arquivo_recebido, TAM_MAX_NOME);
                                jogo.tesouros[i].encontrado = true;
                                jogo.tesouros[i].pos.x = jogo.jogador.x;
                                jogo.tesouros[i].pos.y = jogo.jogador.y;
                                
                                break;
                            }
                        }
                        
                        // Marcar que o grid precisa ser atualizado
                        atualizacao_pendente = true;
                        pthread_mutex_unlock(&mutex_jogo);
                        
                    }
                    pthread_mutex_unlock(&mutex_recebimento);
                    break;
                    
                case TIPO_TAMANHO:
                    // Recebeu informação de tamanho do arquivo
                    if (tam_dados >= sizeof(size_t) && dados != NULL) {
                        size_t tamanho;
                        memcpy(&tamanho, dados, sizeof(size_t));
                        
                        printf("Tamanho do arquivo a receber: %zu bytes\n", tamanho);
                        
                        // Verificar espaço disponível
                        if (verifica_espaco_disponivel(DIRETORIO_RECEBIDOS, tamanho)) {
                            // Enviar ACK
                            enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                                        TIPO_ACK, seq, NULL, 0);
                        } else {
                            // Enviar NACK com erro de espaço insuficiente
                            unsigned char erro = ERRO_ESPACO_INSUF;
                            enviar_pacote(sockfd, &endereco_servidor, mac_servidor, mac_cliente, 
                                        TIPO_NACK, seq, &erro, 1);
                        }
                        
                        // Atualizar último sequencial recebido
                        ultimo_seq_recebido = seq;
                    }
                    break;
                
                default:
                    // Ignora outros tipos de pacotes
                    break;
            }
        }
        
        // Pequena pausa para não sobrecarregar o processador
        usleep(10000); // 10ms
    }
    
    printf("Thread de recebimento finalizada.\n");
    return NULL;
}

// Iniciar o recebimento de um arquivo
bool iniciar_recebimento_arquivo(const char *nome_arquivo) {
    char caminho[512]; // Aumentado para 512 bytes
    snprintf(caminho, sizeof(caminho), "%s/%s", DIRETORIO_RECEBIDOS, nome_arquivo);
    
    // Fechamos qualquer arquivo que esteja aberto
    if (arquivo_recebendo != NULL) {
        fclose(arquivo_recebendo);
        arquivo_recebendo = NULL;
    }
    
    printf("Tentando criar arquivo para escrita: %s\n", caminho);
    
    // Verificar se o diretório existe
    struct stat st = {0};
    if (stat(DIRETORIO_RECEBIDOS, &st) == -1) {
        perror("Diretório recebidos/ não existe");
        printf("Tentando criar o diretório...\n");
        
        if (mkdir(DIRETORIO_RECEBIDOS, 0777) == -1) {
            perror("Falha ao criar diretório");
            // Tentar via sistema
            char cmd[1024]; // Aumentado para 1024 bytes
            snprintf(cmd, sizeof(cmd), "mkdir -p %s && chmod 777 %s", DIRETORIO_RECEBIDOS, DIRETORIO_RECEBIDOS);
            system(cmd);
        }
    }
    
    // Abrir arquivo para escrita
    arquivo_recebendo = fopen(caminho, "wb");
    if (!arquivo_recebendo) {
        perror("Erro ao criar arquivo");
        printf("Não foi possível criar o arquivo %s\n", caminho);
        
        // Tentar via sistema - aumentamos o buffer para 1024 bytes
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "touch %.490s && chmod 666 %.490s", caminho, caminho);
        system(cmd);
        
        arquivo_recebendo = fopen(caminho, "wb");
        if (!arquivo_recebendo) {
            perror("Segunda tentativa falhou");
            return false;
        }
        printf("Arquivo criado com sucesso via comando do sistema.\n");
    } else {
        printf("Arquivo aberto com sucesso para escrita.\n");
    }
    
    return true;
}

// Finalizar o recebimento de um arquivo
void finalizar_recebimento_arquivo(bool sucesso) {
    if (arquivo_recebendo != NULL) {
        fclose(arquivo_recebendo);
        arquivo_recebendo = NULL;
    }
    
    aguardando_arquivo = false;
    
    if (!sucesso) {
        // Em caso de falha, tentar remover o arquivo incompleto
        char caminho[512]; // Aumentado para 512 bytes
        snprintf(caminho, sizeof(caminho), "%s/%s", DIRETORIO_RECEBIDOS, nome_arquivo_recebido);
        remove(caminho);
    }
}

// Tratamento de sinais para encerramento limpo
void tratar_sinal(int signum) {
    printf("\nSinal %d recebido. Encerrando cliente...\n", signum);
    
    // Fechar qualquer arquivo aberto
    if (arquivo_recebendo != NULL) {
        fclose(arquivo_recebendo);
        arquivo_recebendo = NULL;
        printf("Arquivo aberto fechado durante tratamento de sinal.\n");
    }
    
    // Marcar o programa para encerrar
    em_execucao = false;
    
    // Sinalizar para qualquer espera condicional (como no enviar_movimento)
    pthread_cond_signal(&cond_movimento);
}
