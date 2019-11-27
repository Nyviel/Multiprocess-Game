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

char map[ROWS][COLUMNS];
int clientsId;
int seenCampsite;

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


void * print_map(void * arg);

int main (void)
{
    int mem_id = -1;
    printf("Waiting for server!\n");
    while (1)
    {
        mem_id = shm_open("global_memory", O_RDWR, 0600);
        if (mem_id > 0) break;
        sleep(1);
    }
    initscr();
    noecho();
    curs_set(FALSE);
    noecho();
    keypad(stdscr, TRUE);
    seenCampsite = 0;
    mvprintw(4, 55, "Server's PID: %d", mem_id);
    int status = ftruncate(mem_id, sizeof(struct global_data_t));
    if (status < 0) {perror("ftruncate"); return 1;}

    struct global_data_t * structHandle = (struct global_data_t*)mmap(NULL, 32, PROT_READ | PROT_WRITE, MAP_SHARED, mem_id, 0);
    if (structHandle == MAP_FAILED) {perror("mmap"); return 1;}

    sem_t * client_conn_sem = sem_open("connection_sem", 0);
    if (client_conn_sem == SEM_FAILED) {perror("sem_open client conn"); return 1;}

    //Look for a place for yourself in the clients array
    
    //If this is met then the server is full and this client cannot join
    if (structHandle ->current_connections == MAX_CONNECTIONS)
    {
        mvprintw(0, 0, "%s", "Server full!");
        refresh();
        sleep(5);
        endwin();
        return 1;
    }

    clientsId = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        //If this is met then we have found the first free spot
        if (structHandle ->connected_clients[i] == 0)
        {
            structHandle -> connected_clients[i] = 1;
            clientsId = i + 1;
            structHandle -> current_connections++;
            break;
        }
    }

    //Informs the server that a new client has joined and needs new coords
    sem_post(client_conn_sem);

    pthread_t mapThread;
    pthread_create(&mapThread, NULL, print_map, (void *)structHandle);

    structHandle -> client_data[clientsId - 1].human = 1;
    //Main game loop
    while (1)
    {
        //Getting input and handling it
        int ch;
        ch = getch();

        //Quit the game
        if (ch == 'q' || ch == 'Q')
        {
            structHandle ->current_connections--;
            structHandle ->connected_clients[clientsId - 1] = 0;
            structHandle ->client_data[clientsId - 1].x = -1;
            structHandle ->client_data[clientsId - 1].y = -1;
            structHandle ->client_data[clientsId - 1].coins_brought = 0;
            structHandle ->client_data[clientsId - 1].coins_carried = 0;
            structHandle ->client_data[clientsId - 1].deaths = 0;
            break;
        }

        //Moving the player
        //Only set the direction array if all 4 directions are 0 to avoid stacking.
        if (ch == KEY_UP) //Requested North
        {

            //Check if the direction array isn't set already
            int set = 0;
            for (int i = 0; i < 4; i++)
            {
                if (structHandle -> client_data[clientsId-1].directions[i]) {set = 1; break;};
            }
            if (!set)
            {
                structHandle -> client_data[clientsId - 1].directions[0] = 1;
            }
        }
        
        if (ch == KEY_RIGHT) //Requested East
        {
            //Check if the direction array isn't set already
            int set = 0;
            for (int i = 0; i < 4; i++)
            {
                if (structHandle -> client_data[clientsId-1].directions[i]) {set = 1; break;};
            }
            if (!set)
            {
                structHandle -> client_data[clientsId - 1].directions[1] = 1;
            }
        }

        if (ch == KEY_LEFT) //Requested Left
        {
            //Check if the direction array isn't set already
            int set = 0;
            for (int i = 0; i < 4; i++)
            {
                if (structHandle -> client_data[clientsId-1].directions[i]) {set = 1; break;};
            }
            if (!set)
            {
                structHandle -> client_data[clientsId - 1].directions[2] = 1;
            }
        }

        if (ch == KEY_DOWN) //Requested South
        {
            //Check if the direction array isn't set already
            int set = 0;
            for (int i = 0; i < 4; i++)
            {
                if (structHandle -> client_data[clientsId-1].directions[i]) {set = 1; break;};
            }
            if (!set)
            {
                structHandle -> client_data[clientsId - 1].directions[3] = 1;
            }
        }
    }

    // munmap(structHandle, sizeof(struct global_data_t));
    refresh();
    endwin();
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
        int xoffset = (((struct global_data_t *)arg) -> client_data[clientsId - 1].x) - 2;
        int yoffset = (((struct global_data_t *)arg) -> client_data[clientsId - 1].y) - 2;
        
        int row = 0;
        int col = 0;
        for (int i = yoffset; i < yoffset + SIGHT_RANGE; i++)
        {
            for (int j = xoffset; j < xoffset + SIGHT_RANGE; j++)
            {
                char block = (((struct global_data_t *)arg)->client_data[clientsId-1].map_part[row][col]);
                
                if (block == '|')
                {
                    attron(COLOR_PAIR(2));
                    mvprintw(i, j, "%c", block);
                }
                if (block == '#')
                {
                    attron(COLOR_PAIR(4));
                    mvprintw(i, j, "%c", block);
                }
                if (block == '-')
                {
                    attron(COLOR_PAIR(1));
                    mvprintw(i, j, "%c", ' ');
                }

                if (block == 'A')
                {
                    attron(COLOR_PAIR(6));
                    mvprintw(i, j, "%c", 'A');
                    seenCampsite = 1;
                }

                if (block == 'c')
                {
                    attron(COLOR_PAIR(7));
                    mvprintw(i, j, "%c", 'c');
                }
                if (block == 't')
                {
                    attron(COLOR_PAIR(7));
                    mvprintw(i, j, "%c", 't');
                }
                if (block == 'T')
                {
                    attron(COLOR_PAIR(7));
                    mvprintw(i, j, "%c", 'T');
                }

                if (block == 'D')
                {
                    attron(COLOR_PAIR(8));
                    mvprintw(i, j, "%c", 'D');
                }

                if (block == '*')
                {
                    attron(COLOR_PAIR(9));
                    mvprintw(i, j, "%c", '*');
                }

                //Display the client
                if (row == 2 && col == 2)
                {
                    attron(COLOR_PAIR(3));
                    mvprintw(i, j, "%d", clientsId);
                }

                //Display other clients (if possible)
                for (int g = 0; g < MAX_CONNECTIONS; g++)
                {
                    if (((struct global_data_t *)arg) -> connected_clients[g])
                    {
                        if (g == (clientsId - 1)) continue;

                        int otherplayersX = (((struct global_data_t *)arg)->client_data[g].x);
                        int otherplayersY = (((struct global_data_t *)arg)->client_data[g].y);
                    
                        if (otherplayersX >= xoffset && otherplayersX <= xoffset + SIGHT_RANGE - 1)
                        {
                            if (otherplayersY >= yoffset && otherplayersY <= yoffset + SIGHT_RANGE - 1)
                            {
                                attron(COLOR_PAIR(3));
                                mvprintw(otherplayersY, otherplayersX, "%d", g + 1);
                            }
                        }
                    }
                }

                //Display wild beasts (if possible)
                for (int g = 0; g < ((struct global_data_t *)arg)->current_beasts; g++)
                {
                    int beastX = ((struct global_data_t *)arg)->wild_beasts[g].x;
                    int beastY = ((struct global_data_t *)arg)->wild_beasts[g].y;
                
                    if (beastX >= xoffset && beastX <= xoffset + SIGHT_RANGE - 1)
                    {
                        if (beastY >= yoffset && beastY <= yoffset + SIGHT_RANGE - 1)
                        {
                            attron(COLOR_PAIR(9));
                            mvprintw(beastY, beastX, "%c", '*');
                        }
                    }
                }
                col++;
            }
            row++;
            col = 0;
        }
        
        //Stats and info
        attron(COLOR_PAIR(5));
        mvprintw(0, 54, "Server's PID: %d", 1);
        if (seenCampsite)
        {
            mvprintw(1, 54, "Campsite X/Y: %02d/%02d", ((struct global_data_t *)arg) -> campsite_cords[0], ((struct global_data_t *)arg) -> campsite_cords[1]);
        }
        else mvprintw(1, 54, "Campsite X/Y: unknown");
        mvprintw(2, 54, "Round number: %d", (((struct global_data_t *)arg)-> round));

        mvprintw(5, 54, "Player:");
        mvprintw(6, 54, "Number: %d", clientsId);
        mvprintw(7, 54, "Type: HUMAN");
        mvprintw(8, 54, "X: %d", (((struct global_data_t *)arg)-> client_data[clientsId - 1].x));
        mvprintw(9, 54, "Y: %d", (((struct global_data_t *)arg)-> client_data[clientsId - 1].y));
        mvprintw(10, 54, "Deaths: %d", (((struct global_data_t *)arg)-> client_data[clientsId - 1].deaths));
        mvprintw(11, 54, "Coins found: %d", (((struct global_data_t *)arg)-> client_data[clientsId - 1].coins_carried));
        mvprintw(12, 54, "Coins brought: %d", (((struct global_data_t *)arg)-> client_data[clientsId - 1].coins_brought));

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
        usleep(100000);
        clear();
    }
    return NULL;
}
