#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>

#define ROWS 25
#define COLUMNS 51
#define MAX_CONNECTIONS 4
#define SIGHT_RANGE 5
#define MAX_BEASTS 200

sem_t * client_conn_sem;
sem_t * client_move_sem;
sem_t * map_refresh_sem;
sem_t eh_wake_up_sem;
pthread_mutex_t myMutex = PTHREAD_MUTEX_INITIALIZER;

char map[ROWS][COLUMNS];
int campsiteX, campsiteY;

struct coin_bag_t
{
    int x,y;
    int total_coins;
};

int cur_coinBags = 0;
const int coinBags_size = 200;
struct coin_bag_t coinBags[200];


//Substruct for every client
struct client_data_t
{
    //The directions array, if a value is set the client has requested to move in that direction.
    //0-North
    //1-East
    //2-West
    //3-South
    int directions[4];

    int human;
    int in_bush;
    int deaths;
    int coins_carried;
    int coins_brought;
    int x, y;

    //The part of visible map for each client
    char map_part[SIGHT_RANGE][SIGHT_RANGE];
};

struct wild_beast_t
{
    int x, y;
};

//The main struct in shared memory.
struct global_data_t
{
    //Simple variable for storing current client count.
    int current_connections;

    //Storing current beasts count
    int current_beasts;
    
    //Used for counting rounds
    int round;

    //Filled with zeros at the start, 0 - free id, 1 - taken id
    int connected_clients[MAX_CONNECTIONS];

    int clients_PID[MAX_CONNECTIONS];

    int campsite_cords[2];

    //Holds the map data and moving logisitics for each client.
    struct client_data_t client_data[MAX_CONNECTIONS];
    
    //Holds the map data and moving logisitics for each client.
    struct wild_beast_t wild_beasts[MAX_BEASTS];
};

int load_map (char * filename);
void * print_map (void * arg);
void * listen_to_connections (void * arg);
void * update_client (void * arg);
void * event_handler (void * arg);
void * wild_beast(void * arg);

int main (void)
{
    if (load_map("map.txt")) {perror("Failed to load map!\n"); return 1;}
    srand(time(NULL));
    initscr();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(FALSE);
    //Create the shared memory
    int mem_id = shm_open("global_memory", O_CREAT | O_RDWR, 0600);
    if (mem_id < 0) {perror("shm_open"); return 1;}

    //Truncate the size of shared memory
    int status = ftruncate(mem_id, sizeof(struct global_data_t));
    if (status < 0) {perror("ftruncate"); return 1;}

    //Map the shared memory
    struct global_data_t * structHandle = (struct global_data_t*)mmap(NULL, 32, PROT_READ | PROT_WRITE, MAP_SHARED, mem_id, 0);
    if (structHandle == MAP_FAILED) {perror("mmap"); return 1;}

    //Set the values of created struct in shared memory
    memset(structHandle -> connected_clients, 0, MAX_CONNECTIONS);
    structHandle -> current_connections = 0;
    structHandle -> round = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        structHandle -> clients_PID[i] = rand()%2000;
    }

    // Set the values of substruct for each client
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        (structHandle -> client_data[i]).in_bush = 0;
        (structHandle -> client_data[i]).deaths = 0;
        (structHandle -> client_data[i]).coins_brought = 0;
        (structHandle -> client_data[i]).coins_carried = 0;
        (structHandle -> client_data[i]).x = -1;
        (structHandle -> client_data[i]).y = -1;
        memset((structHandle -> client_data[i]).directions, 0, 4);    
    }

    client_conn_sem = sem_open("connection_sem", O_CREAT, 0600, 0);
    if (client_conn_sem == SEM_FAILED) {perror("sem_open client_conn"); return 1;}

    client_move_sem = sem_open("move_sem", O_CREAT, 0600, 0);
    if (client_move_sem == SEM_FAILED) {perror("sem_open client move"); return 1;}

    map_refresh_sem = sem_open("refresh_sem", O_CREAT, 0600, 0);
    if (map_refresh_sem == SEM_FAILED) {perror("sem_open refresh_sem"); return 1;}

    sem_init(&eh_wake_up_sem, 0, 0);

    //Part responsible for listening to new client connections and setting their coordinates
    pthread_t clientConnectionThread;
    pthread_create(&clientConnectionThread, NULL, listen_to_connections, (void *)structHandle);

    //Part responsible for updating each client, move, send map etc.
    pthread_t updateClientThread;
    pthread_create(&updateClientThread, NULL, update_client, (void *)structHandle);
    
    //Map printing thread
    pthread_t mapThread;
    pthread_create(&mapThread, NULL, print_map, (void *)structHandle);

    //Even handling thread
    pthread_t eventThread;
    pthread_create(&eventThread, NULL, event_handler, (void *)structHandle);

    //Create campsite
    while (1)
    {
        campsiteY = rand()%ROWS;
        campsiteX = rand()%COLUMNS;
        if (map[campsiteY][campsiteX] == '-') break;
    }
    map[campsiteY][campsiteX] = 'A';
    structHandle -> campsite_cords[0] = campsiteX;
    structHandle -> campsite_cords[1] = campsiteY;
    
    for (int i = 0; i < coinBags_size; i++)
    {
        coinBags[i].x = -2;
        coinBags[i].y = -2;
        coinBags[i].total_coins = 0;
    }

    sem_post(map_refresh_sem);
    //Main game loop
    while (1)
    {
        //Detect key presses so server can spawn entities
        int ch;
        ch = getch();

        //Quit the game
        if (ch == 'q' || ch == 'Q') break;

        //Spawn a wild beast
        if (ch == 'b' || ch == 'B')
        {
            pthread_t newBeast;
            pthread_create(&newBeast, NULL, wild_beast, (void *)structHandle);
        }

        //Spawn a single coin
        if (ch == 'c')
        {
            //Get random coords
            int coinX, coinY;
            while (1)
            {
                coinX = rand()%COLUMNS;
                coinY = rand()%ROWS;
                if (map[coinY][coinX] == '-') break;
            }
            map[coinY][coinX] = 'c';
            sem_post(map_refresh_sem);
        }

        //Spawn a small treasure
        if (ch == 't')
        {
            //Get random coords
            int treasureX, treasureY;
            while (1)
            {
                treasureX = rand()%COLUMNS;
                treasureY = rand()%ROWS;
                if (map[treasureY][treasureX] == '-') break;
            }
            map[treasureY][treasureX] = 't';
            sem_post(map_refresh_sem);
        }

        //Spawn a large treasure
        if (ch == 'T')
        {
            int treasureX, treasureY;
            while (1)
            {
                treasureX = rand()%COLUMNS;
                treasureY = rand()%ROWS;
                if (map[treasureY][treasureX] == '-') break;
            }
            map[treasureY][treasureX] = 'T';
            sem_post(map_refresh_sem);
        }

    }

    //Clean up the shm
    munmap(structHandle, sizeof(struct global_data_t));
    close(mem_id);
    shm_unlink("global_memory");

    //Clean up the semaphores
    sem_close(client_conn_sem);
    sem_unlink("connection_sem");
    sem_close(client_move_sem);
    sem_unlink("move_sem");
    sem_close(map_refresh_sem);
    sem_unlink("refresh_sem");
    sem_destroy(&eh_wake_up_sem);
    pthread_mutex_destroy(&myMutex);
    refresh();
    endwin();
    return 0;
}

int load_map(char * filename)
{
    if (!filename) return 1;

    FILE * f = fopen(filename, "r");
    if (!f) return 1;

    int current_row = 0;

    while (!feof(f))
    {
        if (feof(f)) break;
        fscanf(f, "%s", map[current_row++]);
        current_row %= ROWS;
    }

    fclose(f);
    return 0;
}

void * print_map(void * arg)
{
    start_color();

    init_pair(1, COLOR_WHITE, COLOR_WHITE);
    init_pair(2, COLOR_BLACK, COLOR_BLACK);
    init_pair(3, COLOR_WHITE, COLOR_MAGENTA);
    init_pair(4, COLOR_BLACK, COLOR_WHITE);
    init_pair(5, COLOR_WHITE, COLOR_BLACK);
    init_pair(6, COLOR_YELLOW, COLOR_GREEN);
    init_pair(7, COLOR_WHITE, COLOR_YELLOW);
    init_pair(8, COLOR_GREEN, COLOR_YELLOW);
    init_pair(9, COLOR_RED, COLOR_WHITE);
    
    while (1)
    {
        sem_wait(map_refresh_sem);

        for (int i = 0; i < ROWS; i++)
        {
            for (int j = 0; j < COLUMNS; j++)
            {
                if (map[i][j] == '|')
                {
                    attron(COLOR_PAIR(2));
                    mvprintw(i, j, "%c", map[i][j]);
                }
                if (map[i][j] == '#')
                {
                    attron(COLOR_PAIR(4));
                    mvprintw(i, j, "%c", map[i][j]);
                }
                //Empty spaces
                if (map[i][j] == '-') 
                {
                    attron(COLOR_PAIR(1));
                    mvprintw(i, j, "%c", ' ');
                }
                if (map[i][j] == 'A')
                {
                    attron(COLOR_PAIR(6));                    
                    mvprintw(i, j, "%c", 'A');
                }
                if (map[i][j] == 'c')
                {
                    attron(COLOR_PAIR(7));
                    mvprintw(i, j, "%c", 'c');
                }
                if (map[i][j] == 't')
                {
                    attron(COLOR_PAIR(7));
                    mvprintw(i, j, "%c", 't');
                }
                if (map[i][j] == 'T')
                {
                    attron(COLOR_PAIR(7));
                    mvprintw(i, j, "%c", 'T');
                }

                if (map[i][j] == 'D')
                {
                    attron(COLOR_PAIR(8));
                    mvprintw(i, j, "%c", 'D');
                }
            }
        }

        //Players
        for (int k = 0; k < ((struct global_data_t *)arg)->current_connections; k++)
        {
            int clientX = ((struct global_data_t *)arg)->client_data[k].x;
            int clientY = ((struct global_data_t *)arg)->client_data[k].y;
            attron(COLOR_PAIR(3));
            mvprintw(clientY, clientX, "%d", k + 1);
        }

        //Beasts
        for (int k = 0; k < ((struct global_data_t *)arg) -> current_beasts; k++)
        {
            int beastX = ((struct global_data_t *)arg) -> wild_beasts[k].x;
            int beastY = ((struct global_data_t *)arg) -> wild_beasts[k].y;
            attron(COLOR_PAIR(9));
            mvprintw(beastY, beastX, "%c", '*');
        }

        //Stats and info
        attron(COLOR_PAIR(5));
        mvprintw(0, 54, "Server's PID: %d", 1);
        mvprintw(1, 54, "Campsite X/Y: %02d/%02d", campsiteX, campsiteY);
        mvprintw(2, 54, "Round number: %d", (((struct global_data_t *)arg)-> round));

        mvprintw(5, 54, "Parameters:");
        mvprintw(5, 69, "Player1:");
        mvprintw(5, 79, "Player2:");
        mvprintw(5, 89, "Player3:");
        mvprintw(5, 99, "Player4:");
        
        mvprintw(6, 55, "PID: ");
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                mvprintw(6, 70 + i * 10, "%d", ((struct global_data_t *)arg)->clients_PID[i]);
            }
            else
            {
                mvprintw(6, 70 + i * 10, "-");
            }
        }

        mvprintw(7, 55, "TYPE: ");
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {

            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                if (((struct global_data_t *)arg)->client_data[i].human) mvprintw(7, 70 + i * 10, "HUMAN");
                else mvprintw(7 ,70 + i * 10, "CPU");
            }
            else
            {
                mvprintw(7, 70 + i * 10, "-");
            }
        }

        mvprintw(8, 55, "CURR X/Y: ");
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                mvprintw(8, 70 + i * 10, "%02d/%02d", ((struct global_data_t *)arg)-> client_data[i].x, ((struct global_data_t *)arg)->client_data[i].y);
            }
            else
            {
                mvprintw(8, 70 + i * 10, "--/--");
            }
        }

        mvprintw(9, 55, "DEATHS: ");
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                mvprintw(9, 70 + i * 10, "%d", (((struct global_data_t *)arg)-> client_data[i].deaths));
            }
            else
            {
                mvprintw(9, 70 + i * 10, "-");
            }
        }
        
        mvprintw(10, 54, "Coins");
        mvprintw(11, 55, "CARRIED: ");
        mvprintw(12, 55, "BROUGHT: ");

        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                mvprintw(11, 70 + i * 10, "%d", (((struct global_data_t *)arg)-> client_data[i].coins_carried));
                mvprintw(12, 70 + i * 10, "%d", (((struct global_data_t *)arg)-> client_data[i].coins_brought));
            }
            else
            {
                mvprintw(11, 70 + i * 10, "-");
                mvprintw(12, 70 + i * 10, "-");
            }
        }

        mvprintw(16, 54, "Legend:");
        mvprintw(17, 54, "1234 - Players");
        mvprintw(18, 54, "| - Wall:");
        mvprintw(19, 54, "# - Bushes");
        mvprintw(20, 54, "* - Enemy:");
        mvprintw(21, 54, "c - One coin");
        mvprintw(22, 54, "t - Treasure (10 coins)");
        mvprintw(23, 54, "T - Large treasure (50 coins)");
        mvprintw(24, 54, "A - Campsite");
        refresh();
        usleep(1000);
        clear();
    }
    return NULL;
}

void * listen_to_connections(void * arg)
{
    //Ever listening thread, check for requests from clients to randomize their coords

    while (1)
    {
        //Waits for signals from client processes
        //This semaphore will only be triggered if client has found a spot
        sem_wait(client_conn_sem);

        //Client has a request
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (((struct global_data_t *)arg)->connected_clients[i]) //Someone is occupying this spot, check coords to find out if its a fresh connection
            {
                //If this if is met then we found the new client
                if (((struct global_data_t *)arg)->client_data[i].x == -1 || ((struct global_data_t *)arg)->client_data[i].y == -1)
                {
                    //Now properly randomize the client's coordinates
                    //Coordinates can't place the client in a wall
                    int x, y;
                    while (1)
                    {
                        x = rand()%COLUMNS;
                        y = rand()%ROWS;

                        if (map[y][x] == '-') break;
                    }
                    ((struct global_data_t *)arg)->client_data[i].x = x;
                    ((struct global_data_t *)arg)->client_data[i].y = y;

                    //Update the clients partial map
                    int xrange = -2;
                    int yrange = -2;
                    int clientx = ((struct global_data_t *)arg)->client_data[i].x;
                    int clienty = ((struct global_data_t *)arg)->client_data[i].y;
                    for (int y = 0; y < SIGHT_RANGE; y++)
                    {
                        for (int x = 0; x < SIGHT_RANGE; x++)
                        {
                            int xparam = clientx + (xrange + x);
                            int yparam = clienty + (yrange + y);
                            if (xparam < 0) xparam = 0;
                            if (xparam >= COLUMNS) xparam = COLUMNS - 1;
                            if (yparam < 0) yparam = 0;
                            if (yparam > ROWS) yparam = ROWS - 1; 
                            ((struct global_data_t *)arg)->client_data[i].map_part[y][x] = map[yparam][xparam];
                        }
                    }
                    break;
                }
            }
        }
        sem_post(map_refresh_sem);
    }
    return NULL;
}

//Updates clients for as long as the game lasts
void * update_client(void * arg)
{
    while (1)
    {
        //Wait for request
        sem_wait(client_move_sem);

        //Move client part
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            int inbush = 0;
            //If this client is connected
            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                // //Check for bush penalty
                // if (map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] == '#')
                // {
                //     ((struct global_data_t *)arg)->client_data[i].in_bush = 1;
                // }
                // //Bush penalty
                // if (((struct global_data_t *)arg)->client_data[i].in_bush)
                // {
                //     //Wait for one second
                //     ((struct global_data_t *)arg)->client_data[i].in_bush = 0;
                //     inbush = 1;
                // }

                //Check for the direction array for each connected client, perhaps they requested a move
                for (int j = 0; j < 4; j++)
                {
                    //If this if has been met the client requested to move in some direction
                    if (((struct global_data_t *)arg)->client_data[i].directions[j])
                    {
                        if (j == 0) //Requested north
                        {
                            //Verify if the move is a possible one
                            int Y = ((struct global_data_t *)arg)->client_data[i].y - 1;
                            int X = ((struct global_data_t *)arg)->client_data[i].x;
                            ((struct global_data_t *)arg)->client_data[i].directions[j] = 0;
                            if (map[Y][X] == '|') //Illegal move
                            {
                                break;    
                            }
                            
                            if (!inbush) ((struct global_data_t *)arg)->client_data[i].y -= 1;
                        }
                        else if (j == 1) //Requested east
                        {
                            //Verify if the move is a possible one
                            int Y = ((struct global_data_t *)arg)->client_data[i].y;
                            int X = ((struct global_data_t *)arg)->client_data[i].x + 1;
                            ((struct global_data_t *)arg)->client_data[i].directions[j] = 0;
                            if (map[Y][X] == '|') //Illegal move
                            {
                                break;    
                            }
                            if (!inbush) ((struct global_data_t *)arg)->client_data[i].x += 1;
                        }
                        else if (j == 2) //Requested west
                        {
                            //Verify if the move is a possible one
                            int Y = ((struct global_data_t *)arg)->client_data[i].y;
                            int X = ((struct global_data_t *)arg)->client_data[i].x - 1;
                            ((struct global_data_t *)arg)->client_data[i].directions[j] = 0;
                            if (map[Y][X] == '|') //Illegal move
                            {
                                break;    
                            }
                            if (!inbush) ((struct global_data_t *)arg)->client_data[i].x -= 1;
                        }
                        else if (j == 3) //Requested south
                        {
                            //Verify if the move is a possible one
                            int Y = ((struct global_data_t *)arg)->client_data[i].y + 1;
                            int X = ((struct global_data_t *)arg)->client_data[i].x;
                            ((struct global_data_t *)arg)->client_data[i].directions[j] = 0;
                            if (map[Y][X] == '|') //Illegal move, straight to jail
                            {
                                break;    
                            }
                            if (!inbush) ((struct global_data_t *)arg)->client_data[i].y += 1;       
                        }
                    }

                }

                //Update the clients partial map
                int xrange = -2;
                int yrange = -2;
                int clientx = ((struct global_data_t *)arg)->client_data[i].x;
                int clienty = ((struct global_data_t *)arg)->client_data[i].y;
                for (int y = 0; y < SIGHT_RANGE; y++)
                {
                    for (int x = 0; x < SIGHT_RANGE; x++)
                    {
                        int xparam = clientx + (xrange + x);
                        int yparam = clienty + (yrange + y);
                        if (xparam < 0) xparam = 0;
                        if (xparam >= COLUMNS) xparam = COLUMNS - 1;
                        if (yparam < 0) yparam = 0;
                        if (yparam > ROWS) yparam = ROWS - 1; 
                        ((struct global_data_t *)arg)->client_data[i].map_part[y][x] = map[yparam][xparam];
                    }
                }
            } 
        }
        sleep(1);

        if (((struct global_data_t *)arg)->current_connections) ((struct global_data_t *)arg) -> round++;
        sem_post(&eh_wake_up_sem);
        sem_post(map_refresh_sem);
    }
    return NULL;
}

//Handles events in the game such as coins colletions, collisions with other players, etc...
void * event_handler(void * arg)
{
    while (1)
    {
        sem_wait(&eh_wake_up_sem);
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            //If this was met there is a client to check
            if (((struct global_data_t *)arg)->connected_clients[i])
            {   
                //Check for coins or treasure and clear after collecting it
                if (map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] == 'c')
                {
                    ((struct global_data_t *)arg)->client_data[i].coins_carried += 1;
                    map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] = '-';
                }   
                
                if (map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] == 't')
                {
                    ((struct global_data_t *)arg)->client_data[i].coins_carried += 10;
                    map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] = '-';
                }

                if (map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] == 'T')
                {
                    ((struct global_data_t *)arg)->client_data[i].coins_carried += 50;
                    map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] = '-';
                }

                //Check for bringing the coins to campsite
                if (map[((struct global_data_t *)arg)->client_data[i].y][((struct global_data_t *)arg)->client_data[i].x] == 'A')
                {
                    ((struct global_data_t *)arg)->client_data[i].coins_brought += ((struct global_data_t *)arg)->client_data[i].coins_carried;
                    ((struct global_data_t *)arg)->client_data[i].coins_carried = 0;
                }

                //Check for collecting existing coin bags
                int mainX = ((struct global_data_t *)arg)->client_data[i].x;
                int mainY = ((struct global_data_t *)arg)->client_data[i].y;
                for (int j = 0; j < coinBags_size; j++)
                {
                    int curX = coinBags[j].x;
                    int curY = coinBags[j].y;

                    //Client is standing in the bag
                    if (mainX == curX && mainY == curY)
                    {
                        map[curY][curX] = '-';
                        ((struct global_data_t *)arg)->client_data[i].coins_carried += coinBags[j].total_coins;
                        coinBags[j].x = -2;
                        coinBags[j].y = -2;
                        coinBags[j].total_coins = 0;
                        cur_coinBags--;
                    }
                }

                //Check for client-client collision
                for (int j = 0; j < MAX_CONNECTIONS; j++)
                {
                    if (((struct global_data_t *)arg)-> connected_clients[j])
                    {
                        if (i == j) continue;

                        int otherX = ((struct global_data_t *)arg)->client_data[j].x;
                        int otherY = ((struct global_data_t *)arg)->client_data[j].y;
                        
                        //Collision occured
                        if ((mainX == otherX && mainY == otherY) && mainX != -1)
                        {
                            //Spawn a coin bag at the collision point with a sum of both players carried coins
                            int first_player_coins = ((struct global_data_t *)arg) -> client_data[i].coins_carried;
                            int second_player_coins = ((struct global_data_t *)arg) -> client_data[j].coins_carried;

                            int coinBagval = first_player_coins + second_player_coins;

                            coinBags[cur_coinBags].total_coins = coinBagval;
                            coinBags[cur_coinBags].x = mainX;
                            coinBags[cur_coinBags].y = mainY;
                            cur_coinBags++;
                            cur_coinBags %= coinBags_size;
                            map[mainY][mainX] = 'D';
                            //Reset the clients coords and reset their current gold carried
                            int newX, newY;
                            while (1)
                            {
                                newX = rand()%COLUMNS;
                                newY = rand()%ROWS;
                                if (map[newY][newX] == '-') break;
                            }
                            ((struct global_data_t *)arg)->client_data[i].x = newX;
                            ((struct global_data_t *)arg)->client_data[i].y = newY;

                            while (1)
                            {
                                newX = rand()%COLUMNS;
                                newY = rand()%ROWS;
                                if (map[newY][newX] == '-') break;
                            }
                            ((struct global_data_t *)arg) -> client_data[j].x = newX;   
                            ((struct global_data_t *)arg) -> client_data[j].y = newY;

                            ((struct global_data_t *)arg) -> client_data[i].coins_carried = 0;   
                            ((struct global_data_t *)arg) -> client_data[j].coins_carried = 0;

                            
                            //Increment the death counter for each client
                            ((struct global_data_t *)arg)->client_data[i].deaths++;
                            ((struct global_data_t *)arg)->client_data[j].deaths++;
                            break;
                        }
                    }

                }
            }
        }
        usleep(50000);
        sem_post(map_refresh_sem);
    }

    return NULL;
}

//This needs a complete rework
void * wild_beast(void * arg)
{
    int wild_beasts_x;
    int wild_beasts_y;
    while (1)
    {
        wild_beasts_x = rand()%COLUMNS;
        wild_beasts_y = rand()%ROWS;
        if (map[wild_beasts_y][wild_beasts_x] == '-') break;
    }
    int id;
    pthread_mutex_lock(&myMutex);
    id = ((struct global_data_t *)arg)->current_beasts;
    (((struct global_data_t *)arg)->wild_beasts[((struct global_data_t *)arg)->current_beasts].x = wild_beasts_x);
    (((struct global_data_t *)arg)->wild_beasts[((struct global_data_t *)arg)->current_beasts].y = wild_beasts_y);
    ((struct global_data_t *)arg)->current_beasts++;
    pthread_mutex_unlock(&myMutex);
    int player_in_range = 0;
    while (1)
    {
        //Primary function of the wild beast is to keep looking for enemies withing its sight
        //The sight of the beast is the same as a player so a 5x5 square
        wild_beasts_x = ((struct global_data_t *)arg)->wild_beasts[id].x;
        wild_beasts_y = ((struct global_data_t *)arg)->wild_beasts[id].y;
        int xoffset = wild_beasts_x - 2;
        int yoffset = wild_beasts_y - 2;

        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (((struct global_data_t *)arg)->connected_clients[i])
            {
                int playerx = ((struct global_data_t *)arg)->client_data[i].x;
                int playery = ((struct global_data_t *)arg)->client_data[i].y;

                if (playerx >= xoffset && playerx <= xoffset + SIGHT_RANGE - 1)
                {
                    if (playery >= yoffset && playery <= yoffset + SIGHT_RANGE - 1)
                    {
                        //If both are true then there is a player within beasts range
                        //Now we should calculate if we should move towards the player
                        //Granted we can move in his direction
                        player_in_range = 1;
                        int can_see = 0;

                        //Figuring out how the beast can see the player here...
                        if (wild_beasts_x == playerx)
                        {
                            //Draw a line downwards to see if the beast can chase
                            if (wild_beasts_y < playery)
                            {
                                int rayx = wild_beasts_x;
                                int rayy = wild_beasts_y;
                                for (int k = 0; k < (playery-wild_beasts_y); k++)
                                {
                                    int block = map[rayy + k][rayx];
                                    if (block == '|') {can_see = 0; break;}
                                    can_see = 1; 
                                }
                            }
                            //Draw a line upwards to see if the beast can chase
                            if (wild_beasts_y > playery)
                            {
                                int rayx = wild_beasts_x;
                                int rayy = wild_beasts_y;
                                for (int k = 0; k < (wild_beasts_y-playery); k++)
                                {
                                    int block = map[rayy - k][rayx];
                                    if (block == '|') {can_see = 0; break;}
                                    can_see = 1; 
                                }
                            }
                        }

                        if (wild_beasts_y == playery)
                        {
                            //Draw a line to the left to see if the beast can chase
                            if (wild_beasts_x > playerx)
                            {
                                int rayx = wild_beasts_x;
                                int rayy = wild_beasts_y;
                                for (int k = 0; k < (wild_beasts_x-playerx); k++)
                                {
                                    int block = map[rayy][rayx - k];
                                    if (block == '|') {can_see = 0; break;}
                                    can_see = 1; 
                                }
                            }
                            //Draw a line to the right to see if the beast can chase
                            if (wild_beasts_x < playerx)
                            {
                                int rayx = wild_beasts_x;
                                int rayy = wild_beasts_y;
                                for (int k = 0; k < (playerx - wild_beasts_x); k++)
                                {
                                    int block = map[rayy][rayx + k];
                                    if (block == '|') {can_see = 0; break;}
                                    can_see = 1; 
                                }
                            }
                        }

                        if (can_see)
                        {
                            //Since beast can see the player on the map we can handle the moving logic
                            if (wild_beasts_x < playerx)
                            {
                                //If this is true we should move the beast to the right on the map
                                //Only if the move itself is possible, meaning there is no wall inbetween

                                char block = map[wild_beasts_y][wild_beasts_x + 1];
                                if (block != '|')
                                {
                                    ((struct global_data_t *)arg) -> wild_beasts[id].x++;
                                }
                            }                            

                            if (wild_beasts_x > playerx)
                            {
                                //If this is true we should move the beast to the left on the map
                                //Only if the move itself is possible, meaning there is no wall inbetween
                                
                                char block = map[wild_beasts_y][wild_beasts_x - 1];
                                if (block != '|')
                                {
                                    ((struct global_data_t *)arg) -> wild_beasts[id].x--;
                                }
                                
                            }

                            if (wild_beasts_y > playery)
                            {
                                //If this is true we should move the beast up on the map
                                //Only if the move itself is possible, meaning there is no wall inbetween
                                
                                char block = map[wild_beasts_y - 1][wild_beasts_x];
                                if (block != '|')
                                {
                                    ((struct global_data_t *)arg) -> wild_beasts[id].y--;
                                }
                            }   

                            if (wild_beasts_y < playery)
                            {
                                //If this is true we should move the beast down on the map
                                //Only if the move itself is possible, meaning there is no wall inbetween

                                char block = map[wild_beasts_y + 1][wild_beasts_x];
                                if (block != '|')
                                {
                                    ((struct global_data_t *)arg) -> wild_beasts[id].y++;
                                }
                            }   
                        }


                        //Collision check
                        if (wild_beasts_x == playerx && wild_beasts_y == playery)
                        {
                            //Collision occured
                            //Increment players death count, randomize his coordinates and reset his carried coins
                            //Create a coin bag at the place of collision
                            ((struct global_data_t *)arg)->client_data[i].deaths++;
                            int newX;
                            int newY;
                            while (1)
                            {
                                newX = rand()%COLUMNS;
                                newY = rand()%ROWS;
                                if (map[newY][newX] == '-') break;
                            }
                            ((struct global_data_t *)arg)->client_data[i].x = newX;
                            ((struct global_data_t *)arg)->client_data[i].y = newY;

                            coinBags[cur_coinBags].total_coins = ((struct global_data_t *)arg)->client_data[i].coins_carried;
                            coinBags[cur_coinBags].x = wild_beasts_x;
                            coinBags[cur_coinBags].y = wild_beasts_y;
                            cur_coinBags++;
                            cur_coinBags %= coinBags_size;
                            map[wild_beasts_y][wild_beasts_x] = 'D';

                            ((struct global_data_t *)arg)->client_data[i].coins_carried = 0;
                        }
                    } else player_in_range = 0;
                } else player_in_range = 0;
            }
        }
        
        if (!player_in_range)
        {
            int randDir = rand()%4;
            if (randDir == 0)
            {
                char block = map[wild_beasts_y - 1][wild_beasts_x];
                if (block != '|')
                {
                    ((struct global_data_t *)arg)->wild_beasts[id].y--;
                }
            }
            if (randDir == 1)
            {
                char block = map[wild_beasts_y][wild_beasts_x + 1];
                if (block != '|')
                {
                    ((struct global_data_t *)arg)->wild_beasts[id].x++;
                }
            }
            if (randDir == 2)
            {
                char block = map[wild_beasts_y][wild_beasts_x - 1];
                if (block != '|')
                {
                    ((struct global_data_t *)arg)->wild_beasts[id].x--;
                }
            }
            if (randDir == 3)
            {
                char block = map[wild_beasts_y + 1][wild_beasts_x];
                if (block != '|')
                {
                    ((struct global_data_t *)arg)->wild_beasts[id].y++;
                }
            }
        }
        sem_post(map_refresh_sem);

        sleep(1);
    }

    return NULL;
}