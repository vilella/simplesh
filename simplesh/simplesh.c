/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: APELLIDOS, NOMBRE (GX.X)
 *          APELLIDOS, NOMBRE (GX.X)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <libgen.h>
#include <stdbool.h>
#include <math.h>


// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>


/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


static const char* VERSION = "0.18";

static pid_t pidd; // Para guardar el pid del proceso en segundo plano

// Niveles de depuración
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


// Número máximo de argumentos de un comando
#define MAX_ARGS 16

//2^20 -> Veo mejor la opción de 1024*1024, creo que es menor carga para el sistema
#define MAX_BSIZE (1024 * 1024)


// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";


/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parámetros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de órdenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
            break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *después* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);


// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
	// Ejercicio 1
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, S_IRWXU, 0);
                break;
            case '>':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU, 1);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDWR|O_CREAT|O_APPEND, S_IRWXU, 1);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}


/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/

/* Funciones realizadas por los alumnos */

// Ejercicio3
void run_cwd(){
  char ruta[PATH_MAX];

  if (!getcwd(ruta, PATH_MAX)){
    perror("getcwd");
    exit(EXIT_FAILURE);
  }

  fprintf(stdout, "cwd: %s\n",ruta);

}

// Ejercicio 4
void run_exit(){
	exit(EXIT_SUCCESS);
}


void run_cd(struct cmd* cmd){

    struct execcmd* ecmd;
	ecmd = (struct execcmd*) cmd;
	char * path = ecmd->argv[1];
	char ruta[PATH_MAX];
	getcwd(ruta, PATH_MAX);

    //printf("Estructura %s - %d\n", ecmd->argv[1], ecmd->argc);
    if (ecmd->argc > 2){
        fprintf(stderr, "run_cd: Demasiados argumentos\n");
        return;
    }

	// Si path es NULL nos movemos al directorio home
	if (path == NULL){

		if (chdir(getenv("HOME")) == -1){
			perror("run_cd");
		}

	} else if (strcmp(path,"-") == 0){

		if (chdir(getenv("OLDPWD")) == -1){

			fprintf(stderr, "run_cd: Variable OLDPWD no definida\n");

		}

	} else if (chdir(path) == -1){

        fprintf(stderr, "run_cd: No existe el directorio '%s'\n", path);

    }

	if (setenv("OLDPWD", ruta, 1) == -1){
		perror("run_cd");
	}

}


// Comando hd
void run_hd(struct execcmd* ecmd)
{
  int B_SIZE = 1024;
  int opt;
  int flagl, flagb;
  flagl = flagb = 0;
  optind = 1;
  int numlineas,numbytes;
  numlineas = numbytes = 0;
  int descriptor_fichero = 0;

  // Antes de usar read o write hay que usar open para abrir el fichero, open te devulve el descriptor
  // de fichero y con eso puedes apuntar al fichero que se abre

  while ((opt = getopt(ecmd->argc,ecmd->argv, "l:b:t:h")) != -1) {
    switch (opt) {
      case 'l':
      flagl= 1;
      numlineas = atoi(optarg);
      break;
      case 'b':
      flagb = 1;
      numbytes = atoi(optarg);
      break;
      case 'h':
      fprintf(stdout,"Uso: hd [-l NLINES] [-b NBYTES] [-t BSIZE] [FILE1] [FILE2 ]... \n");
      fprintf(stdout,"\tOpciones: \n");
      fprintf(stdout,"\t-l NLINES Numero maximo de lineas a mostrar. \n");
      fprintf(stdout,"\t-b NBYTES Numero maximo de  bytes a mostrar. \n");
      fprintf(stdout,"\t-t BSIZE   Tamaño en  bytes  de los  bloques  leidos de [FILEn] o stdin. \n");
      fprintf(stdout,"\t-h help \n");
      return;
      break;
      case 't':
      B_SIZE = atoi(optarg);
      if(1 > B_SIZE || B_SIZE > MAX_BSIZE){
        fprintf(stderr, "hd: Opcion no valida");
        B_SIZE=1024;
      }
      break;
      default:
      fprintf(stderr, "Usage: %s [-f] [-n NUM]\n", ecmd->argv[0]);
      exit(EXIT_FAILURE);
    }
  }


  char buf[B_SIZE];

  // Cuando el usuario introduce las opciones -l y -b
  if (flagb == 1 && flagl == 1){
    fprintf(stderr, "hd: Opciones incompatibles\n");
    return;
  }

  int numOpciones = 0;
  if (flagb == 1){
    numOpciones+=2;
  }
  if (flagl == 1){
    numOpciones+=2;
  }

  int numFicheros = ((ecmd->argc) - numOpciones - 1);
  int fd[numFicheros];

  if(numFicheros==0){ //Si no recibe argumentos y solo se pulsa hd se leia de la entrada standard (preguntar profesor)
  
    if (flagb == 0 && flagl == 0) {
        int lineas = 0;
        while(lineas < 3){
          int leidos = read(0,buf,B_SIZE); //pongo 0 porque es el descriptor de fichero de la entrada standard
          write(1,buf,leidos); //Se lee lo que se escribe de la entrada standard
          lineas++;
        }
    }
    // FIXME Añadir opciones -l y -b cuando no hay ficheros
  } else {

    for (int j = 0; j < numFicheros; j++){
      fd[j] = open(ecmd->argv[ecmd->argc-numFicheros+j],O_RDONLY);
      if(fd[j] == -1){
        perror("open");
        exit(EXIT_FAILURE);
      }

    }

    if(flagl==1){ //con opcion l
      for (int i = 0; i < numFicheros; i++){
        descriptor_fichero = fd[i];
        int saltos = 0;
        int indice = 0;
        int bytesleidos=0;
        while ((bytesleidos = read(descriptor_fichero,buf,B_SIZE)) != EOF && saltos < numlineas){
          for(indice=0; indice < bytesleidos && saltos < numlineas; indice++){
            if(buf[indice]=='\n'){    
              saltos++;
            }
            write(1,buf,indice);//FIXME problema al escribir los bytes leidos
            fprintf(stdout,"\n");
          }
        }
        
      }
    }

    if(flagb==1){ //con opcion b
      for (int i = 0; i < numFicheros; i++){

        descriptor_fichero = fd[i];
        int bytesleidos = 0;
        while (bytesleidos <= numbytes && (bytesleidos = read(descriptor_fichero,buf,numbytes)) != EOF){

          if(bytesleidos > numbytes){
            write(1,buf,numbytes);
          }
          bytesleidos += read(descriptor_fichero,buf,B_SIZE);
        }
      }
    }
  }
}


// Comando src
void run_src(struct execcmd* ecmd){

    int opt, flagd, n;
    optind = 1;
    flagd = 0;
    char comentario = '%';
    int numOpciones = 0;
    int numFicheros = 0;
    int bytesLeidos = 0;
    int B_SIZE = 1024;
    char buf[B_SIZE];
    
    while ((opt = getopt(ecmd->argc, ecmd->argv, "d:h")) != -1){
        switch(opt){
            case 'h':
                fprintf(stdout, "Uso: src [-d DELIM] [FILE1] [FILE2]...\n");
                fprintf(stdout, "\tOpciones:\n");
                fprintf(stdout, "\t-d DELIM Caracteres de inicio de comentarios.\n");
                fprintf(stdout, "\t-h help\n");
                break;
           case 'd':
                flagd = 1;
                if (strlen(optarg) != 1){
                    fprintf(stderr, "src: Opcion no valida\n");
                    return;
                }
                comentario = optarg[0];

                numOpciones+=2;
           break;
           
        }
    }
    
    numFicheros = ecmd->argc-numOpciones;
    int i = 0;
    if (numFicheros == 0){
          int lineas = 0;
        while(lineas < 3){
          int leidos = read(0,buf,B_SIZE); //pongo 0 porque es el descriptor de fichero de la entrada standard
          write(1,buf,leidos); //Se lee lo que se escribe de la entrada standard
          lineas++;
        }
        
    }

    
    

}

bool checkInterno(struct cmd* cmd){

	struct execcmd* ecmd;
	ecmd = (struct execcmd*) cmd;

  if (strcmp(ecmd->argv[0], "cwd") == 0){
    return(true);
  } else if (strcmp(ecmd->argv[0], "exit") == 0){
    return(true);
  } else if (strcmp(ecmd->argv[0], "cd") == 0){
    return true;
  } else if (strcmp(ecmd->argv[0], "hd") == 0){
    return true;
  } else if (strcmp(ecmd->argv[0], "src") == 0){
    return true;
  }

  return (false);
}

void ejecutarFuncion(struct cmd* cmd){

	struct execcmd* ecmd;
	ecmd = (struct execcmd*) cmd;

    if (strcmp(ecmd->argv[0], "cwd") == 0){
        run_cwd();
    } else if (strcmp(ecmd->argv[0], "exit") == 0){
        run_exit();
    } else if (strcmp(ecmd->argv[0], "cd") == 0){
        run_cd(cmd);
    } else if (strcmp(ecmd->argv[0], "hd") == 0){
        run_hd(ecmd);
    } else if (strcmp(ecmd->argv[0], "src") == 0){
        run_src(ecmd);
    }

}

//EJERCICIO 1: Tratamiento de las señales. Bloqueo SIGINT y SIGQUIT
void ignorarSignal(){

    // Para ignorar SIGQUIT
   sigset_t ignorar_signal;
   sigemptyset(&ignorar_signal);
   sigaddset(&ignorar_signal, SIGQUIT);
	
   if (sigprocmask(SIG_SETMASK, &ignorar_signal, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
   }
}

void bloquearSignal(){

    // Para bloquear SIGINT
   sigset_t bloquear_signal;
   sigemptyset(&bloquear_signal);
   sigaddset(&bloquear_signal, SIGINT);
	
   if (sigprocmask(SIG_BLOCK, &bloquear_signal, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
   }
}

void manejadorSignals(){
   
   ignorarSignal();
   bloquearSignal();
   		
}

/****************************************/

void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == 0) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}


void run_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd;
    int B_SIZE = 1024;
    char buf[B_SIZE];

    // Para restablcer la redirección
    int saved_stdout;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;


    // FIXME Todos los wait(null) cambiar por waitpid(pid)
    switch(cmd->type)
    {
        case EXEC:

            ecmd = (struct execcmd*) cmd;
            
			// Comprobar si es interno
            if(ecmd->argv[0] != NULL && checkInterno(cmd)){

                ejecutarFuncion(cmd);

            }else{

              if (fork_or_panic("fork EXEC") == 0){
                exec_cmd(ecmd);
              }
              TRY( wait(NULL) );

            }

            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            

            if (fork_or_panic("fork REDR") == 0)
            {
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }
                //FIXME arreglar la redirección
                TRY( dup2(0, fd) );
                TRY( close(rcmd->fd) );
                
                if (checkInterno(rcmd->cmd)){
                    ejecutarFuncion(rcmd->cmd);
                } else {
                    if (rcmd->cmd->type == EXEC){
                        exec_cmd((struct execcmd*) rcmd->cmd);
                    }else{
                        run_cmd(rcmd->cmd);
                    }
    			}

    			// Deshacer la redirección
			    // PISTA: utilizar dup2();
			    // Restaurar stdout
			    //dup2(fd, 0);
    			//close(fd);
			    exit(EXIT_SUCCESS);

            }



            TRY( wait(NULL) );
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            run_cmd(lcmd->left);
            run_cmd(lcmd->right);

            break;

        case PIPE:
            pcmd = (struct pipecmd*)cmd;
            if (pipe(p) < 0)
            {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            // Ejecución del hijo de la izquierda
            if (fork_or_panic("fork PIPE left") == 0)
            {
                TRY( close(1) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );

				// Comprobar
				if(checkInterno(pcmd->left)){

					ejecutarFuncion(pcmd->left);

            	} else {

		        	if (pcmd->left->type == EXEC)
		        	    exec_cmd((struct execcmd*) pcmd->left);
		        	else
		            	run_cmd(pcmd->left);

				}

				exit(EXIT_SUCCESS);

			}

            // Ejecución del hijo de la derecha
            if (fork_or_panic("fork PIPE right") == 0)
            {
                TRY( close(0) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );

				if(checkInterno(pcmd->right)){

					ejecutarFuncion(pcmd->right);

            	} else {

			        if (pcmd->right->type == EXEC)
			            exec_cmd((struct execcmd*) pcmd->right);
			        else
			            run_cmd(pcmd->right);

				}

				 exit(EXIT_SUCCESS);

			}

            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos
            TRY( wait(NULL) );   // waitpid();
            TRY( wait(NULL) );   // 
            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
            
            if ((pidd = fork_or_panic("fork BACK")) == 0)
            {
                printf("\nHago fork\n");
                printf("pid: %d\n", pidd);
                // Despues del fork ejercicio 2 [PID]
                fprintf(stdout, "[%d]", pidd);
                // Almacenar PID en un array tam8
				if(checkInterno(bcmd->cmd)){

					ejecutarFuncion(bcmd->cmd);

            	} else {

		            if (bcmd->cmd->type == EXEC)
		                exec_cmd((struct execcmd*) bcmd->cmd);
		            else
		                run_cmd(bcmd->cmd);
				}

                exit(EXIT_SUCCESS);
            }
            
            fprintf(stdout, "[%d]", pidd);
            
            TRY( waitpid(pidd, NULL, 0) );
            // Hacer wait para esperar al hijo  --> waitpid(pid);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            if (fork_or_panic("fork SUBS") == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( wait(NULL) );
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}


void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);

        case EXEC:
            ecmd = (struct execcmd*) cmd;
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;
    }
}


void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
//	    	free(cmd);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            free(rcmd->cmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            free(lcmd->right);
            free(lcmd->left);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            free(pcmd->right);
            free(pcmd->left);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            free(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            free(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char* get_cmd(){
  char* buf;

  char ruta[PATH_MAX];

  uid_t uid = getuid();

  struct passwd * pw = getpwuid(uid);

  if (pw == NULL){
    perror("getpwuid");
    exit(EXIT_FAILURE);
  }

  if (!getcwd(ruta, PATH_MAX)){
    perror("getcwd");
    exit(EXIT_FAILURE);
  }

  char * directorio = basename(ruta);

  char prompt[strlen(pw->pw_name)+strlen(directorio)+strlen("@< ")];

  sprintf(prompt, "%s@%s> ", pw->pw_name,directorio);

  // Lee la orden tecleada por el usuario
  buf = readline(prompt);

  // Si el usuario ha escrito una orden, almacenarla en la historia.
  if(buf)
  add_history(buf);

  return buf;
}


/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(int argc, char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argc, argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }
}


int main(int argc, char** argv)
{
    char* buf;
    struct cmd* cmd;

    // FIXME Hacer el ejercicio 1 de las señales
    manejadorSignals();
    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");

	// FIXME Borrar variable de entorno OLDPWD???
	unsetenv("OLDPWD");

    // Bucle de lectura y ejecución de órdenes
    while ((buf = get_cmd()) != NULL)
    {
        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la línea de órdenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);

        // Libera la raíz del árbol
        free(cmd);

        // Libera la memoria de la línea de órdenes
        free(buf);

    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}
