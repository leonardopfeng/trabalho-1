#include "treasure_protocol.h"

// Função para imprimir um buffer em hexadecimal (para debug)
void print_buffer(const char* prefix, unsigned char* buffer, int size) {
    printf("%s", prefix);
    for (int i = 0; i < size; i++) {
        printf("%02X ", buffer[i]);
    }
    printf("\n");
}

// Função para calcular o checksum simples
unsigned char calcula_checksum(unsigned char* dados, int tamanho) {
    unsigned short soma = 0;  // Usando unsigned short para evitar overflow
    for (int i = 0; i < tamanho; i++) {
        soma += dados[i];  // Soma os bytes
    }
    return (unsigned char)(soma & 0xFF);  // Retorna apenas o byte menos significativo
}

// Função para criar um socket raw
int cria_raw_socket(char* interface) {
    // Cria arquivo para o socket sem qualquer protocolo
    int soquete = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (soquete == -1) {
        fprintf(stderr, "Erro ao criar socket: Verifique se você é root!\n");
        exit(-1);
    }
 
    int ifindex = if_nametoindex(interface);
    if (ifindex == 0) {
        fprintf(stderr, "Interface %s não encontrada.\n", interface);
        exit(-1);
    }
 
    struct sockaddr_ll endereco = {0};
    endereco.sll_family = AF_PACKET;
    endereco.sll_protocol = htons(ETH_P_ALL);
    endereco.sll_ifindex = ifindex;
    
    // Inicializa socket
    if (bind(soquete, (struct sockaddr*) &endereco, sizeof(endereco)) == -1) {
        fprintf(stderr, "Erro ao fazer bind no socket\n");
        exit(-1);
    }
 
    struct packet_mreq mr = {0};
    mr.mr_ifindex = ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    
    if (setsockopt(soquete, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) == -1) {
        fprintf(stderr, "Erro ao fazer setsockopt: "
            "Verifique se a interface de rede foi especificada corretamente.\n");
        exit(-1);
    }
 
    return soquete;
}

// Função para enviar um pacote
bool enviar_pacote(int sockfd, struct sockaddr_ll *endereco, unsigned char *mac_destino, 
                  unsigned char *mac_origem, unsigned char tipo, unsigned char seq, 
                  unsigned char *dados, int tam_dados) {
    
    if (tam_dados > TAM_MAX_DADOS) {
        fprintf(stderr, "Erro: Tamanho de dados excede o máximo permitido.\n");
        return false;
    }
    
    // Buffer para o pacote completo
    unsigned char pacote[TAM_MAX_PACOTE];
    struct ether_header *eth = (struct ether_header *)pacote;
    
    // Configurar o cabeçalho Ethernet
    memcpy(eth->ether_dhost, mac_destino, 6);
    memcpy(eth->ether_shost, mac_origem, 6);
    eth->ether_type = htons(ETH_CUSTOM_TYPE);
    
    // Ponteiro para a parte de dados do pacote (após o cabeçalho Ethernet)
    unsigned char *payload = pacote + sizeof(struct ether_header);
    
    // Configurar o cabeçalho do protocolo
    payload[0] = MARCADOR;            // Marcador de início
    payload[1] = tam_dados & 0x7F;    // Tamanho (7 bits)
    payload[2] = seq & 0x1F;          // Sequência (5 bits)
    payload[3] = tipo & 0x0F;         // Tipo (4 bits)
    
    // Cria um buffer temporário para calcular o checksum
    unsigned char temp_buffer[TAM_MAX_PACOTE];
    memcpy(temp_buffer, payload + 1, 3);           // Copia tamanho, seq e tipo
    
    // Se houver dados, copia para o buffer temporário
    if (tam_dados > 0 && dados != NULL) {
        memcpy(temp_buffer + 3, dados, tam_dados);
    }
    
    // Calcula e armazena o checksum
    unsigned char checksum = calcula_checksum(temp_buffer, 3 + tam_dados);
    payload[4] = checksum;
    
    // Copia os dados para o pacote
    if (tam_dados > 0 && dados != NULL) {
        memcpy(payload + 5, dados, tam_dados);
    }
    
    // Tamanho total do pacote
    size_t tam_total = sizeof(struct ether_header) + 5 + tam_dados;
    
    // Envia o pacote
    ssize_t enviados = sendto(sockfd, pacote, tam_total, 0, 
                             (struct sockaddr*)endereco, sizeof(struct sockaddr_ll));
    
    if (enviados < 0) {
        perror("sendto");
        return false;
    }
    
    return (enviados == (ssize_t)tam_total);
}

// Função para receber um pacote
bool receber_pacote(int sockfd, unsigned char *buffer, unsigned char *tipo, 
                   unsigned char *seq, unsigned char **dados, int *tam_dados) {
    
    struct sockaddr_ll addr;
    socklen_t addr_len = sizeof(addr);
    
    // Recebe um pacote
    ssize_t n = recvfrom(sockfd, buffer, TAM_MAX_PACOTE, 0, 
                         (struct sockaddr*)&addr, &addr_len);
    
    if (n < 0) {
        perror("recvfrom");
        return false;
    }
    
    // Verifica se o pacote tem tamanho mínimo para ser um pacote válido
    if ((size_t)n < sizeof(struct ether_header) + 5) {
        return false;
    }
    
    struct ether_header *eth = (struct ether_header *)buffer;
    
    // Verifica se é do nosso protocolo
    if (ntohs(eth->ether_type) != ETH_CUSTOM_TYPE) {
        return false;
    }
    
    unsigned char *payload = buffer + sizeof(struct ether_header);
    
    // Verifica o marcador
    if (payload[0] != MARCADOR) {
        return false;
    }
    
    // Extrai informações do cabeçalho
    *tam_dados = payload[1] & 0x7F;
    *seq = payload[2] & 0x1F;
    *tipo = payload[3] & 0x0F;
    unsigned char checksum_recebido = payload[4];
    
    // Verifica o checksum
    unsigned char temp_buffer[TAM_MAX_PACOTE];
    memcpy(temp_buffer, payload + 1, 3);            // Copia os 3 bytes de cabeçalho
    
    // Se houver dados, copia para o buffer temporário
    if (*tam_dados > 0) {
        memcpy(temp_buffer + 3, payload + 5, *tam_dados);
    }
    
    unsigned char checksum_calculado = calcula_checksum(temp_buffer, 3 + *tam_dados);
    
    if (checksum_recebido != checksum_calculado) {
        return false;
    }
    
    // Define o ponteiro para os dados
    *dados = payload + 5;
    
    return true;
}

// Função para verificar o espaço disponível em um diretório
bool verifica_espaco_disponivel(const char* diretorio, size_t tamanho_necessario) {
    struct statvfs stat;
    
    if (statvfs(diretorio, &stat) != 0) {
        perror("statvfs");
        return false;
    }
    
    // Calcula o espaço livre em bytes
    size_t espaco_livre = stat.f_bsize * stat.f_bavail;
    
    // Adiciona uma margem de segurança (10%)
    size_t tamanho_com_margem = tamanho_necessario * 1.1;
    
    return (espaco_livre >= tamanho_com_margem);
}

// Função para obter o tipo de um arquivo com base em sua extensão
int obter_tipo_arquivo(const char *nome_arquivo) {
    const char *extensao = strrchr(nome_arquivo, '.');
    
    if (extensao == NULL) {
        return TIPO_ARQ_DESCONHECIDO;
    }
    
    if (strcasecmp(extensao, ".txt") == 0) {
        return TIPO_ARQ_TEXTO;
    } else if (strcasecmp(extensao, ".mp4") == 0) {
        return TIPO_ARQ_VIDEO;
    } else if (strcasecmp(extensao, ".jpg") == 0 || strcasecmp(extensao, ".jpeg") == 0) {
        return TIPO_ARQ_IMAGEM;
    }
    
    return TIPO_ARQ_DESCONHECIDO;
}

// Função para inicializar o estado do jogo
void inicializar_jogo(EstadoJogo *jogo) {
    // Inicializa posição do jogador no canto inferior esquerdo
    jogo->jogador.x = 0;
    jogo->jogador.y = 0;
    
    // Limpa os grids
    memset(jogo->grid_visitado, 0, sizeof(jogo->grid_visitado));
    memset(jogo->grid_tesouro, 0, sizeof(jogo->grid_tesouro));
    
    // Marca a posição inicial como visitada
    jogo->grid_visitado[0][0] = true;
    
    // Inicializa os tesouros
    for (int i = 0; i < NUM_TESOUROS; i++) {
        jogo->tesouros[i].encontrado = false;
        sprintf(jogo->tesouros[i].nome, "%d", i + 1); // será completado depois
    }
    
    // Gera posições aleatórias para os tesouros
    srand(time(NULL));
    for (int i = 0; i < NUM_TESOUROS; i++) {
        bool posicao_valida = false;
        while (!posicao_valida) {
            int x = rand() % GRID_SIZE;
            int y = rand() % GRID_SIZE;
            
            // Verifica se a posição já tem um tesouro
            if (!jogo->grid_tesouro[y][x]) {
                jogo->tesouros[i].pos.x = x;
                jogo->tesouros[i].pos.y = y;
                jogo->grid_tesouro[y][x] = true;
                posicao_valida = true;
            }
        }
    }
}

// Função para mover o jogador
bool mover_jogador(EstadoJogo *jogo, int direcao) {
    int novo_x = jogo->jogador.x;
    int novo_y = jogo->jogador.y;
    
    // Calcula a nova posição com base na direção
    switch (direcao) {
        case TIPO_MOVE_DIR:   // Direita
            novo_x++;
            break;
        case TIPO_MOVE_ESQ:   // Esquerda
            novo_x--;
            break;
        case TIPO_MOVE_CIMA:  // Cima
            novo_y++;
            break;
        case TIPO_MOVE_BAIXO: // Baixo
            novo_y--;
            break;
        default:
            return false;
    }
    
    // Verifica se a nova posição está dentro dos limites do grid
    if (novo_x < 0 || novo_x >= GRID_SIZE || novo_y < 0 || novo_y >= GRID_SIZE) {
        return false;
    }
    
    // Atualiza a posição do jogador
    jogo->jogador.x = novo_x;
    jogo->jogador.y = novo_y;
    
    // Marca a posição como visitada
    jogo->grid_visitado[novo_y][novo_x] = true;
    
    return true;
}

// Função para verificar se há um tesouro na posição atual do jogador
int verificar_tesouro(EstadoJogo *jogo) {
    int x = jogo->jogador.x;
    int y = jogo->jogador.y;
    
    // Verifica se há um tesouro na posição
    if (jogo->grid_tesouro[y][x]) {
        // Procura qual tesouro está nesta posição
        for (int i = 0; i < NUM_TESOUROS; i++) {
            if (jogo->tesouros[i].pos.x == x && jogo->tesouros[i].pos.y == y) {
                if (!jogo->tesouros[i].encontrado) {
                    jogo->tesouros[i].encontrado = true;
                    return i + 1; // Retorna o índice do tesouro (1-based)
                }
                return 0; // Tesouro já encontrado
            }
        }
    }
    
    return 0; // Nenhum tesouro encontrado
} 