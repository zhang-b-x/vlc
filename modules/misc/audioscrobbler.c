/*****************************************************************************
 * audioscrobbler.c : audioscrobbler submission plugin
 *****************************************************************************
 * Copyright (C) 2006 the VideoLAN team
 * $Id$
 *
 * Authors: Rafa�l Carr� <rafael -dot- carre -at- gmail -dot- com>
 *          Kenneth Ostby <kenneo -at- idi -dot- ntnu -dot- no>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#define _GNU_SOURCE
#include <string.h>

#if !defined( strlwr ) && !defined( WIN32 )
#include <ctype.h>
#endif

#if defined( WIN32 )
#include <time.h>
#endif
/*
 * TODO :
 * review/replace :
 *   hexa()
 *   md5 to char[]
 */
#include <vlc/vlc.h>
#include <vlc/intf.h>
#include <vlc_meta.h>
#include <vlc_md5.h>
#include <vlc_block.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <network.h>

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

/* Keeps track of metadata to be submitted, and if song has been submitted */
typedef struct audioscrobbler_song_t
{
    char        *psz_a;                /* track artist     */
    char        *psz_t;                /* track title      */
    char        *psz_b;                /* track album      */
    int         i_l;                   /* track length     */
/* vlc can't retrieve musicbrainz id, so let's ignore it   */
/*  int         i_m;  */               /* musicbrainz id   */
    char        *psz_i;                /* date             */
    time_t      time_playing;          /* date (epoch)     */
} audioscrobbler_song_t;


/* Queue to be submitted to server, 10 songs max */
typedef struct audioscrobbler_queue_t
{
    audioscrobbler_song_t   **p_queue;      /* contains up to 10 songs        */
    int                     i_songs_nb;     /* number of songs                */
    void                    *p_next_queue;  /* if queue full, pointer to next */
} audioscrobbler_queue_t;

struct intf_sys_t
{
    audioscrobbler_queue_t  *p_first_queue;     /* 1st queue              */
    vlc_mutex_t             lock;               /* p_sys mutex            */

/* data about audioscrobbler session */
    int                     i_interval;         /* last interval recorded */
    time_t                  time_last_interval; /* when was it recorded ? */
    char                    *psz_submit_host;   /* where to submit data ? */
    int                     i_submit_port;      /* at which port ?        */
    char                    *psz_submit_file;   /* in which file ?        */
    char                    *psz_username;      /* last.fm username       */
    vlc_bool_t              b_handshaked;       /* did we handshake ?     */
    int                     i_post_socket;      /* socket for submission  */
    char                    *psz_response_md5;  /* md5 response to use    */

/* data about input elements */
    input_thread_t          *p_input;           /* previous p_input       */
    audioscrobbler_song_t   *p_current_song;    /* song being played      */
    time_t                  time_pause;         /* time when vlc paused   */
    time_t                  time_total_pauses;  /* sum of time in pause   */
    vlc_bool_t              b_queued;           /* has it been queud ?    */
    vlc_bool_t              b_metadata_read;    /* did we read metadata ? */
    vlc_bool_t              b_paused;           /* are we playing ?       */
};

intf_sys_t *p_sys_global;     /* for use same p_sys in all threads */

static int  Open    ( vlc_object_t * );
static void Close   ( vlc_object_t * );
static void Run     ( intf_thread_t * );
static int ItemChange   ( vlc_object_t *, const char *,
                        vlc_value_t, vlc_value_t, void * );
static int PlayingChange( vlc_object_t *, const char *,
                        vlc_value_t, vlc_value_t, void * );
static int AddToQueue   ( intf_thread_t *p_this );
static int Handshake    ( intf_thread_t *p_sd );
static int ReadMetaData ( intf_thread_t *p_this );
void DeleteQueue( audioscrobbler_queue_t *p_queue );
char *hexa( short int i );

#if !defined(strlwr) && !defined( WIN32 )
char* strlwr(char *psz_string);
#endif

/*****************************************************************************
 * Module descriptor
 ****************************************************************************/


#define APPLICATION_NAME "VLC media player"
#define USERNAME_TEXT N_("Username")
#define USERNAME_LONGTEXT N_("Audioscrobbler username")
#define PASSWORD_TEXT N_("Password")
#define PASSWORD_LONGTEXT N_("Audioscrobbler password")
#define DEFAULT_INTERVAL 60
#define CLIENT_NAME     PACKAGE
#define CLIENT_VERSION  VERSION

/* HTTP POST request : to submit data */
#define    POST_REQUEST "POST /%s HTTP/1.1\n"                               \
                        "Accept-Encoding: identity\n"                       \
                        "Content-length: %d\n"                              \
                        "Connection: close\n"                               \
                        "Content-type: application/x-www-form-urlencoded\n" \
                        "Host: %s\n"                                        \
                        "User-agent: VLC Media Player/%s\r\n"               \
                        "\r\n"                                              \
                        "%s\r\n"                                            \
                        "\r\n"

/* data to submit */
#define POST_DATA "u=%s&s=%s&a%%5B%d%%5D=%s&t%%5B%d%%5D=%s" \
                  "&b%%5B%d%%5D=%s&m%%5B%d%%5D=&l%%5B%d%%5D=%d&i%%5B%d%%5D=%s"

vlc_module_begin();
    set_category( CAT_INTERFACE );
    set_subcategory( SUBCAT_INTERFACE_CONTROL );
    set_shortname( N_( "Audioscrobbler" ) );
    set_description( _("Audioscrobbler submission Plugin") );
    add_string( "lastfm-username", "", NULL,
                USERNAME_TEXT, USERNAME_LONGTEXT, VLC_TRUE );
    add_string( "lastfm-password", "", NULL,
                PASSWORD_TEXT, PASSWORD_LONGTEXT, VLC_TRUE );
    set_capability( "interface", 0 );
    set_callbacks( Open, Close );
vlc_module_end();

/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/

static int Open( vlc_object_t *p_this )
{
    intf_thread_t       *p_intf;
    intf_sys_t          *p_sys;
    playlist_t          *p_playlist;

    p_intf = ( intf_thread_t* ) p_this;
    p_sys = malloc( sizeof( intf_sys_t ) );
    if( !p_sys )
    {
      goto error;
    }

    vlc_mutex_init( p_this, &p_sys->lock );

    p_sys_global = p_sys;
    p_sys->psz_submit_host = NULL;
    p_sys->psz_submit_file = NULL;
    p_sys->b_handshaked = VLC_FALSE;
    p_sys->i_interval = 0;
    p_sys->time_last_interval = time( NULL );
    p_sys->psz_username = NULL;
    p_sys->p_input = NULL;
    p_sys->b_paused = VLC_FALSE;

    /* md5 response is 32 chars, + final \0 */
    p_sys->psz_response_md5 = malloc( sizeof( char ) * 33 );
    if( !p_sys->psz_response_md5 )
    {
        vlc_mutex_destroy ( &p_sys->lock );
        goto error;
   }

    p_sys->p_first_queue = malloc( sizeof( audioscrobbler_queue_t ) );
    if( !p_sys->p_first_queue )
    {
        vlc_mutex_destroy( &p_sys->lock );
        goto error;
    }

    p_sys->p_current_song = malloc( sizeof( audioscrobbler_song_t ) );
    if( !p_sys->p_current_song )
    {
        vlc_mutex_destroy( &p_sys->lock );
        goto error;
    }

    /* queues can't contain more than 10 songs */
    p_sys->p_first_queue->p_queue =
        malloc( 10 * sizeof( audioscrobbler_song_t ) );
    if( !p_sys->p_current_song )
    {
        vlc_mutex_destroy( &p_sys->lock );
        goto error;
    }

    p_sys->p_first_queue->i_songs_nb = 0;
    p_sys->p_first_queue->p_next_queue = NULL;

    p_playlist = pl_Yield( p_intf );
    var_AddCallback( p_playlist, "playlist-current", ItemChange, p_intf );
    pl_Release( p_playlist );

    p_intf->pf_run = Run;

    return VLC_SUCCESS;

error:
    free( p_sys->p_current_song );
    free( p_sys->p_first_queue );
    free( p_sys->psz_response_md5 );
    free( p_sys );

    return VLC_ENOMEM;
}

/*****************************************************************************
 * Close: destroy interface stuff
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    audioscrobbler_queue_t      *p_current_queue, *p_next_queue;
    input_thread_t              *p_input;
    intf_thread_t               *p_intf = (intf_thread_t* ) p_this;
    intf_sys_t                  *p_sys = p_intf->p_sys;
    playlist_t                  *p_playlist = pl_Yield( p_intf );

    p_playlist = pl_Yield( p_intf );
    var_DelCallback( p_playlist, "playlist-current", ItemChange, p_intf );

    PL_LOCK;
    p_input = p_playlist->p_input;
    if( p_input ) vlc_object_yield( p_input );
    PL_UNLOCK;

    if( p_input )
    {
        var_DelCallback( p_input, "state", PlayingChange, p_intf );
    }

    pl_Release( p_playlist );

    vlc_mutex_lock ( &p_sys->lock );
    p_current_queue = p_sys->p_first_queue;
    vlc_mutex_unlock ( &p_sys->lock );

    while( ( p_current_queue->i_songs_nb == 10 ) &&
        ( p_current_queue->p_next_queue != NULL ) )
    {
        p_next_queue = p_current_queue->p_next_queue;
        DeleteQueue( p_current_queue );
        free( p_current_queue );
        p_current_queue = p_next_queue;
    }

    DeleteQueue( p_current_queue );
    free( p_current_queue );

    vlc_mutex_lock ( &p_sys->lock );
    if ( p_sys->psz_username )
    {
        free( p_sys->psz_username );
    }

    free( p_sys->p_current_song );
    vlc_mutex_unlock ( &p_sys->lock );
    vlc_mutex_destroy( &p_sys->lock );
    free( p_sys );
}

/*****************************************************************************
 * Run : Handshake with audioscrobbler, then submit songs
 *****************************************************************************/
static void Run( intf_thread_t *p_this )
{
    char                    *psz_submit_string = NULL;
    int                     i_handshake;
    int                     i_netprintf;
    int                     i_song;
    playlist_t              *p_playlist;
    uint8_t                 *p_buffer = NULL;
    char                    *p_buffer_pos = NULL;
    time_t                  played_time;
    audioscrobbler_queue_t  *p_first_queue;
    intf_sys_t              *p_sys;

    p_this->p_sys = p_sys_global;
    p_sys = p_this->p_sys;
    while( !p_this->b_die )
    {
        if( p_sys->b_handshaked == VLC_FALSE )
        {
            if ( time( NULL ) >=
                ( p_sys->time_last_interval + p_sys->i_interval ) )
            {
                msg_Dbg( p_this, "Handshaking with last.fm ..." );
                i_handshake = Handshake( p_this );

                if( i_handshake == VLC_ENOMEM )
                {
                    msg_Err( p_this, "Out of memory" );
                    return;
                }

                else if( i_handshake == VLC_ENOVAR )
                /* username not set */
                {
                    msg_Dbg( p_this, "Set an username then restart vlc" );
                    vlc_mutex_unlock ( &p_sys->lock );
                    return;
                }

                else if( i_handshake == VLC_SUCCESS )
                {
                    msg_Dbg( p_this, "Handshake successfull :)" );
                    vlc_mutex_lock ( &p_sys->lock );
                    p_sys->b_handshaked = VLC_TRUE;
                    vlc_mutex_unlock ( &p_sys->lock );
                }

                else
                {
                    vlc_mutex_lock ( &p_sys->lock );
                    p_sys->i_interval = DEFAULT_INTERVAL;
                    time( &p_sys->time_last_interval );
                    vlc_mutex_unlock ( &p_sys->lock );
                }
            }
        }

        else
        {
            if ( ( p_sys->p_first_queue->i_songs_nb > 0 ) &&
                ( time( NULL ) >=
                ( p_sys->time_last_interval + p_sys->i_interval )  ) )
            {
                msg_Dbg( p_this, "Going to submit some data..." );
                vlc_mutex_lock ( &p_sys->lock );
                psz_submit_string = malloc( 2048 * sizeof( char ) );

                if (!psz_submit_string)
                {
                    msg_Err( p_this, "Out of memory" );
                    vlc_mutex_unlock ( &p_sys->lock );
                    return;
                }

                for (i_song = 0; i_song < p_sys->p_first_queue->i_songs_nb ;
                    i_song++ )
                {
                    snprintf( psz_submit_string, 2048, POST_DATA,
                        p_sys->psz_username, p_sys->psz_response_md5,
                        i_song, p_sys->p_first_queue->p_queue[i_song]->psz_a,
                        i_song, p_sys->p_first_queue->p_queue[i_song]->psz_t,
                        i_song, p_sys->p_first_queue->p_queue[i_song]->psz_b,
                        i_song,
                        i_song, p_sys->p_first_queue->p_queue[i_song]->i_l,
                        i_song, p_sys->p_first_queue->p_queue[i_song]->psz_i
                    );
                }

                p_sys->i_post_socket = net_ConnectTCP( p_this,
                    p_sys->psz_submit_host, p_sys->i_submit_port);

                i_netprintf = net_Printf(
                    VLC_OBJECT(p_this), p_sys->i_post_socket, NULL,
                    POST_REQUEST, p_sys->psz_submit_file,
                    strlen( psz_submit_string), p_sys->psz_submit_file,
                    VERSION, psz_submit_string
                );

                if ( i_netprintf == -1 )
                {
                /* If connection fails, we assume we must handshake again */
                    p_sys->i_interval = DEFAULT_INTERVAL;
                    time( &p_sys->time_last_interval );
                    p_sys->b_handshaked = VLC_FALSE;
                    vlc_mutex_unlock ( &p_sys->lock );
                    return;
                }

                p_buffer = ( uint8_t* ) calloc( 1, 1024 );
                if ( !p_buffer )
                {
                    msg_Err( p_this, "Out of memory" );
                    vlc_mutex_unlock ( &p_sys->lock );
                    return;
                }

                net_Read( p_this, p_sys->i_post_socket, NULL,
                          p_buffer, 1024, VLC_FALSE );
                net_Close( p_sys->i_post_socket );

                p_buffer_pos = strstr( ( char * ) p_buffer, "INTERVAL" );

                if ( p_buffer_pos )
                {
                    p_sys->i_interval = atoi( p_buffer_pos +
                                              strlen( "INTERVAL " ) );
                    time( &p_sys->time_last_interval );
                }

                p_buffer_pos = strstr( ( char * ) p_buffer, "FAILED" );

                if ( p_buffer_pos )
                {
                    msg_Err( p_this, p_buffer_pos );
                    vlc_mutex_unlock ( &p_sys->lock );
                    return;
                }

                p_buffer_pos = strstr( ( char * ) p_buffer, "BADAUTH" );

                if ( p_buffer_pos )
                {
                    msg_Err( p_this,
                             "Authentification failed, handshaking again" );
                    p_sys->b_handshaked = VLC_FALSE;
                    vlc_mutex_unlock ( &p_sys->lock );
                    return;
                }

                p_buffer_pos = strstr( ( char * ) p_buffer, "OK" );

                if ( p_buffer_pos )
                {
                    if ( p_sys->p_first_queue->i_songs_nb == 10 )
                    {
                        p_first_queue = p_sys->p_first_queue->p_next_queue;
                        DeleteQueue( p_sys->p_first_queue );
                        free( p_sys->p_first_queue );
                        p_sys->p_first_queue = p_first_queue;
                    }
                    else
                    {
                        DeleteQueue( p_sys->p_first_queue );
                        p_sys->p_first_queue->i_songs_nb = 0;
                    }
                    msg_Dbg( p_this, "Submission successfull!" );
                }
                vlc_mutex_unlock ( &p_sys->lock );
            }
        }
        msleep( INTF_IDLE_SLEEP );

        p_playlist = pl_Yield( p_this );
        PL_LOCK;
        if( p_playlist->request.i_status == PLAYLIST_STOPPED )
        {
            PL_UNLOCK;
            /* if we stopped, we won't submit playing song */
            vlc_mutex_lock( &p_sys->lock );
            p_sys->b_queued = VLC_TRUE;
            p_sys->b_metadata_read = VLC_TRUE;
            vlc_mutex_unlock( &p_sys->lock );
        }
        else
        {
            PL_UNLOCK;
        }

        pl_Release( p_playlist );
        vlc_mutex_lock( &p_sys->lock );

        if( p_sys->b_metadata_read == VLC_FALSE )
        {
            time( &played_time );
            played_time -= p_sys->p_current_song->time_playing;
            played_time -= p_sys->time_total_pauses;


            vlc_mutex_unlock( &p_sys->lock );

            /* ok now we can read meta data */
            if( played_time > 10 )
            {
                ReadMetaData( p_this );
            }
        }
        else
        {
            if( ( p_sys->b_queued == VLC_FALSE )
                && ( p_sys->b_paused == VLC_FALSE ) )
            {
                vlc_mutex_unlock( &p_sys->lock );
                if( AddToQueue( p_this ) == VLC_ENOMEM )
                {
                    msg_Err( p_this, "Out of memory" );
                    return;
                }
            }
            else
            {
                vlc_mutex_unlock( &p_sys->lock );
            }
        }
    }
}

/*****************************************************************************
 * PlayingChange: Playing status change callback
 *****************************************************************************/
static int PlayingChange( vlc_object_t *p_this, const char *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    intf_thread_t       *p_intf;
    intf_sys_t          *p_sys;

    p_intf = ( intf_thread_t* ) p_data;
    p_sys = p_intf->p_sys;

    (void)p_this; (void)psz_var; (void)oldval;

    vlc_mutex_lock( &p_sys->lock );

    if( newval.i_int == PAUSE_S )
    {
        time( &p_sys->time_pause );
        p_sys->b_paused = VLC_TRUE;
    }

    if( newval.i_int == PLAYING_S )
    {
        p_sys->time_total_pauses += time( NULL ) - p_sys->time_pause;
        p_sys->b_paused = VLC_TRUE;
    }

    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * ItemChange: Playlist item change callback
 *****************************************************************************/
static int ItemChange( vlc_object_t *p_this, const char *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    playlist_t          *p_playlist;
    input_thread_t      *p_input = NULL;
    intf_thread_t       *p_intf;
    intf_sys_t          *p_sys;

    time_t              epoch;
    struct tm           *epoch_tm;

    char                psz_date[20];

    (void)p_this; (void)psz_var; (void)oldval; (void)newval;

    p_intf = ( intf_thread_t* ) p_data;
    p_sys = p_intf->p_sys;

    p_playlist = pl_Yield( p_intf );
    PL_LOCK;
    p_input = p_playlist->p_input;

    if( !p_input )
    {
        PL_UNLOCK;
        pl_Release( p_playlist );
        vlc_mutex_lock( &p_sys->lock );

        /* we won't read p_input */
        p_sys->b_queued = VLC_TRUE;
        p_sys->b_metadata_read = VLC_TRUE;

        vlc_mutex_unlock( &p_sys->lock );
        return VLC_SUCCESS;
    }

    vlc_object_yield( p_input );
    PL_UNLOCK;
    pl_Release( p_playlist );

    var_AddCallback( p_input, "state", PlayingChange, p_intf );

    vlc_mutex_lock ( &p_sys->lock );

    /* reset pause counter */
    p_sys->time_total_pauses = 0;

    /* we save the p_input value to delete the callback later */
    p_sys->p_input = p_input;

    /* we'll read after to be sure it's present */
    p_sys->b_metadata_read = VLC_FALSE;

    p_sys->b_queued = VLC_TRUE;

    time( &epoch );
    epoch_tm = gmtime( &epoch );
    snprintf( psz_date, 20, "%.4d-%.2d-%.2d %.2d:%.2d:%.2d",
        epoch_tm->tm_year+1900, epoch_tm->tm_mon+1, epoch_tm->tm_mday,
        epoch_tm->tm_hour, epoch_tm->tm_min, epoch_tm->tm_sec );

    p_sys->p_current_song->psz_i = encode_URI_component( psz_date );
    p_sys->p_current_song->time_playing = epoch;

    p_sys->b_paused = ( p_input->b_dead || !p_input->input.p_item->psz_name )
                      ? VLC_TRUE : VLC_FALSE;

    vlc_mutex_unlock( &p_sys->lock );

    vlc_object_release( p_input );
    return VLC_SUCCESS;
}

/*****************************************************************************
 * AddToQueue: Add the played song to the queue to be submitted
 *****************************************************************************/
static int AddToQueue ( intf_thread_t *p_this )
{
    int                         i_songs_nb;
    time_t                      played_time;
    audioscrobbler_queue_t      *p_queue = NULL, *p_next_queue = NULL;
    intf_sys_t                  *p_sys;

    p_sys = p_this->p_sys;

    /* has it been queued already ? is it long enough ? */
    vlc_mutex_lock( &p_sys->lock );
    if( p_sys->p_current_song->i_l < 30 )
    {
        msg_Dbg( p_this, "Song too short (< 30s) -> not submitting" );
        p_sys->b_queued = VLC_TRUE;
        vlc_mutex_unlock ( &p_sys->lock );
        return VLC_SUCCESS;
    }

    /* wait for the user to listen enough before submitting */
    time ( &played_time );
    played_time -= p_sys->p_current_song->time_playing;
    played_time -= p_sys->time_total_pauses;

    if( ( played_time < 240 )
        && ( played_time < ( p_sys->p_current_song->i_l / 2 ) ) )
    {
        vlc_mutex_unlock ( &p_sys->lock );
        return VLC_SUCCESS;
    }

    msg_Dbg( p_this, "Ok. We'll put it in the queue for submission" );

    p_queue = p_sys->p_first_queue;

    while( ( p_queue->i_songs_nb == 10 ) && ( p_queue->p_next_queue != NULL ) )
    {
        p_queue = p_queue->p_next_queue;
    }

    i_songs_nb = p_queue->i_songs_nb;

    if( i_songs_nb == 10 )
    {
        p_next_queue = malloc( sizeof( audioscrobbler_queue_t ) );
        if( !p_next_queue )
        {
            vlc_mutex_unlock ( &p_sys->lock );
            return VLC_ENOMEM;
        }
        p_queue->p_next_queue = p_next_queue;
        i_songs_nb = 0;
        p_queue = p_next_queue;
        p_queue->i_songs_nb = i_songs_nb;
    }

    p_queue->p_queue[i_songs_nb] = malloc( sizeof( audioscrobbler_song_t ) );

    p_queue->p_queue[i_songs_nb]->i_l = p_sys->p_current_song->i_l;

    p_queue->p_queue[i_songs_nb]->psz_a =
        strdup( p_sys->p_current_song->psz_a );

    p_queue->p_queue[i_songs_nb]->psz_t =
        strdup( p_sys->p_current_song->psz_t );

    p_queue->p_queue[i_songs_nb]->psz_b =
        strdup( p_sys->p_current_song->psz_b );

    p_queue->p_queue[i_songs_nb]->psz_i =
        strdup( p_sys->p_current_song->psz_i );

    p_queue->i_songs_nb++;
    p_sys->b_queued = VLC_TRUE;

    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Handshake : Init audioscrobbler connection
 *****************************************************************************/
static int Handshake( intf_thread_t *p_this )
{
    char                *psz_password = NULL;
    struct md5_s        *p_struct_md5 = NULL;
    char                *psz_password_md5 = NULL;
    char                *ps_challenge_md5 = NULL;

    stream_t            *p_stream;
    char                *psz_handshake_url = NULL;

    uint8_t             *p_buffer = NULL;
    char                *p_buffer_pos = NULL;
    char                *psz_buffer_substring = NULL;
    char                *psz_url_parser = NULL;
    int                 i_url_pos, i;

    char                *b1, *b2, *b3, *b4;

    intf_thread_t       *p_intf;
    intf_sys_t          *p_sys;

    p_intf = ( intf_thread_t* ) p_this;
    p_sys = p_this->p_sys;

    vlc_mutex_lock ( &p_sys->lock );

    p_sys->psz_username = config_GetPsz(p_this, "lastfm-username");
    if ( !p_sys->psz_username )
    {
        goto memerror;
    }


    if ( !*p_sys->psz_username )
    {
        msg_Info( p_this, "You have to set an username! "
         "Visit https://www.last.fm/join/" );
        return VLC_ENOVAR;
    }

    psz_handshake_url = malloc( 1024 );
    if ( !psz_handshake_url )
    {
        goto memerror;
    }

    snprintf( psz_handshake_url, 1024,
        "http://post.audioscrobbler.com/?hs=true&p=1.1&c=%s&v=%s&u=%s",
        CLIENT_NAME, CLIENT_VERSION, p_sys->psz_username );

    p_stream = stream_UrlNew( p_intf, psz_handshake_url);

    free( psz_handshake_url );

    if( !p_stream )
    {
        p_sys->i_interval = DEFAULT_INTERVAL;
        time( &p_sys->time_last_interval );
        vlc_mutex_unlock ( &p_sys->lock );
        return VLC_EGENERIC;
    }

    p_buffer = ( uint8_t* ) calloc( 1, 1024 );
    if ( !p_buffer )
    {
        stream_Delete( p_stream );
        goto memerror;
    }

    if ( stream_Read( p_stream, p_buffer, 1024 ) == 0 )
    {
        stream_Delete( p_stream );
        free( p_buffer );
        vlc_mutex_unlock ( &p_sys->lock );
        return VLC_EGENERIC;
    }

    stream_Delete( p_stream );

    p_buffer_pos = strstr( ( char * ) p_buffer, "INTERVAL" );

    if ( p_buffer_pos )
    {
        p_sys->i_interval = atoi( p_buffer_pos + strlen( "INTERVAL " ) );
        time( &p_sys->time_last_interval );
    }

    p_buffer_pos = strstr( ( char * ) p_buffer, "FAILED" );

    if ( p_buffer_pos )
    {
        msg_Info( p_this, p_buffer_pos );
        free( p_buffer );
        vlc_mutex_unlock ( &p_sys->lock );
        return VLC_EGENERIC;
    }

    p_buffer_pos = strstr( ( char * ) p_buffer, "BADUSER" );

    if ( p_buffer_pos )
    {
        msg_Info( p_intf, "Username is incorrect" );
        free( p_buffer );
        vlc_mutex_unlock ( &p_sys->lock );
        return VLC_EGENERIC;
    }

    p_buffer_pos = strstr( ( char * ) p_buffer, "UPDATE" );

    if ( p_buffer_pos )
    {
        msg_Dbg( p_intf, "Protocol updated" );
        msg_Dbg( p_intf, p_buffer_pos );
    }

    else
    {
        p_buffer_pos = strstr( ( char * ) p_buffer, "UPTODATE" );
        if ( !p_buffer_pos )
        {
            msg_Dbg( p_intf, "Protocol error" );
            free( p_buffer );
            vlc_mutex_unlock ( &p_sys->lock );
            return VLC_EGENERIC;
        }
    }

    psz_buffer_substring = strndup( strstr( p_buffer_pos, "\n" ) + 1, 32 );
    if ( !psz_buffer_substring )
    {
        goto memerror;
    }
    else
    {
        ps_challenge_md5 = malloc( sizeof( char ) * 32 );
        if ( !ps_challenge_md5 )
        {
            goto memerror;
        }
        memcpy( ps_challenge_md5, psz_buffer_substring, 32 );
        free( psz_buffer_substring );
    }

    p_buffer_pos = ( void* ) strstr( ( char* ) p_buffer, "http://" );

    if ( p_sys->psz_submit_host != NULL )
    {
        free( p_sys->psz_submit_host );
    }

    if ( p_sys->psz_submit_file != NULL )
    {
        free( p_sys->psz_submit_file );
    }

    psz_url_parser = p_buffer_pos + strlen( "http://" );

    i_url_pos = strcspn( psz_url_parser, ":" );
    p_sys->psz_submit_host = strndup( psz_url_parser, i_url_pos );

    p_sys->i_submit_port = atoi( psz_url_parser + i_url_pos + 1 );

    psz_url_parser += strcspn( psz_url_parser , "/" ) + 1;
    i_url_pos = strcspn( psz_url_parser, "\n" );
    p_sys->psz_submit_file = strndup( psz_url_parser, i_url_pos );

    free(p_buffer);

    p_struct_md5 = malloc( sizeof( struct md5_s ) );
    if( !p_struct_md5 )
    {
        goto memerror;
    }

    psz_password = config_GetPsz(p_this, "lastfm-password");
    if ( !psz_password )
    {
         goto memerror;
    }

    InitMD5( p_struct_md5 );
    AddMD5( p_struct_md5, ( uint8_t* ) psz_password, strlen( psz_password ) );
    EndMD5( p_struct_md5 );

    free( psz_password );

    psz_password_md5 = malloc ( 33 * sizeof( char ) );
    if ( !psz_password_md5 )
    {
        goto memerror;
    }

    for ( i = 0; i < 4; i++ )
    {
    /* TODO check that this works on every arch/platform (uint32_t to char) */
        b1 = hexa( p_struct_md5->p_digest[i] % 256 );
        b2 = hexa( ( p_struct_md5->p_digest[i] / 256 ) % 256 );
        b3 = hexa( ( p_struct_md5->p_digest[i] / 65536 ) % 256 );
        b4 = hexa( p_struct_md5->p_digest[i] / 16777216 );
        sprintf( &psz_password_md5[8*i], "%s%s%s%s", b1, b2, b3, b4 );
        free( b1 );
        free( b2 );
        free( b3 );
        free( b4 );
    }

    strlwr( psz_password_md5 );

    InitMD5( p_struct_md5 );
    AddMD5( p_struct_md5, ( uint8_t* ) psz_password_md5, 32 );
    AddMD5( p_struct_md5, ( uint8_t* ) ps_challenge_md5, 32 );
    EndMD5( p_struct_md5 );

    free( ps_challenge_md5 );
    free( psz_password_md5 );

    for ( i = 0; i < 4; i++ )
    {
        b1 = hexa( p_struct_md5->p_digest[i] % 256 );
        b2 = hexa( ( p_struct_md5->p_digest[i] / 256 ) % 256 );
        b3 = hexa( ( p_struct_md5->p_digest[i] / 65536 ) % 256 );
        b4 = hexa( p_struct_md5->p_digest[i] / 16777216 );
        sprintf( &p_sys->psz_response_md5[8*i],"%s%s%s%s", b1, b2, b3, b4 );
        free( b1 );
        free( b2 );
        free( b3 );
        free( b4 );
    }

    p_sys->psz_response_md5[32] = 0;

    strlwr( p_sys->psz_response_md5 );

    vlc_mutex_unlock ( &p_sys->lock );

    return VLC_SUCCESS;

memerror:
    free( p_buffer );
    free( p_struct_md5 );
    free( psz_buffer_substring );

    vlc_mutex_unlock ( &p_sys->lock );
    return VLC_ENOMEM;
}

/*****************************************************************************
 * hexa : Converts a byte to a string in its hexadecimal value
 *****************************************************************************/
char *hexa( short int i )
{
    char        *res = calloc( 3 , sizeof( char ) );

    ((i/16) < 10) ? res[0] = (i / 16) + '0' : ( res[0] = (i/16) + 'a' - 10 );
    ((i%16) < 10) ? res[1] = (i % 16) + '0' : ( res[1] = (i%16) + 'a' - 10 );

    return res;
}
/*****************************************************************************
 * strlwr : Converts a string to lower case
 *****************************************************************************/
#if !defined(strlwr) && !defined( WIN32 )
char* strlwr(char *psz_string)
{
    while ( *psz_string )
    {
        *psz_string++ = tolower( *psz_string );
    }
    return psz_string;
}
#endif

/*****************************************************************************
 * DeleteQueue : Free all songs from an audioscrobbler_queue_t
 *****************************************************************************/
void DeleteQueue( audioscrobbler_queue_t *p_queue )
{
    int     i;

    for( i = 0; i < p_queue->i_songs_nb; i++ )
    {
        free( p_queue->p_queue[i]->psz_a );
        free( p_queue->p_queue[i]->psz_b );
        free( p_queue->p_queue[i]->psz_t );
        free( p_queue->p_queue[i]->psz_i );
        free( p_queue->p_queue[i] );
    }
}

/*****************************************************************************
 * ReadMetaData : Puts current song's meta data in p_sys->p_current_song
 *****************************************************************************/
static int ReadMetaData( intf_thread_t *p_this )
{
    playlist_t          *p_playlist;
    char                *psz_title = NULL;
    char                *psz_artist = NULL;
    char                *psz_album = NULL;
    int                 i_length = -1;
    input_thread_t      *p_input = NULL;
    vlc_value_t         video_val;
    intf_sys_t          *p_sys;

    p_sys = p_this->p_sys;
    p_playlist = pl_Yield( p_this );
    PL_LOCK;
    p_input = p_playlist->p_input;

    if( !p_input )
    {
        return VLC_SUCCESS;
    }

    vlc_object_yield( p_input );
    PL_UNLOCK;
    pl_Release( p_playlist );

    if ( p_input->input.p_item->i_type == ITEM_TYPE_NET )
    {
        msg_Dbg( p_this, "We play a stream -> no submission");
        goto no_submission;
    }

    var_Change( p_input, "video-es", VLC_VAR_CHOICESCOUNT, &video_val, NULL );
    if( video_val.i_int > 0 )
    {
        msg_Dbg( p_this, "We play a video -> no submission");
        goto no_submission;
    }

    if ( p_input->input.p_item->p_meta->psz_artist )
    {
        psz_artist = encode_URI_component(
            p_input->input.p_item->p_meta->psz_artist );
        if ( !psz_artist )
        {
            goto error;
        }
    }
    else
    {
        msg_Dbg( p_this, "No artist.." );
        goto no_submission;
    }

    if ( p_input->input.p_item->p_meta->psz_album )
    {
        psz_album = encode_URI_component(
            p_input->input.p_item->p_meta->psz_album );
        if ( !psz_album )
        {
            goto error;
        }
    }
    else
    {
        msg_Dbg( p_this, "No album.." );
        goto no_submission;
    }

    if ( p_input->input.p_item->psz_name )
    {
        psz_title = encode_URI_component( p_input->input.p_item->psz_name );
        if ( !psz_title )
        {
            goto error;
        }
    }
    else
    {
        msg_Dbg( p_this, "No track name.." );
        goto no_submission;
    }

    i_length = p_input->input.p_item->i_duration / 1000000;

    vlc_object_release( p_input );

    vlc_mutex_lock ( &p_sys->lock );

    p_sys->p_current_song->psz_a = strdup( psz_artist );
    p_sys->p_current_song->psz_t = strdup( psz_title );
    p_sys->p_current_song->psz_b = strdup( psz_album );
    p_sys->p_current_song->i_l = i_length;
    p_sys->b_queued = VLC_FALSE;
    p_sys->b_metadata_read = VLC_TRUE;

    vlc_mutex_unlock ( &p_sys->lock );

    msg_Dbg( p_this, "Meta data registered, waiting to be queued" );

    free( psz_title );
    free( psz_artist );
    free( psz_album );

    return VLC_SUCCESS;

error:
    free( psz_artist );
    free( psz_album );
    free( psz_title );
    return VLC_ENOMEM;

no_submission:
    vlc_object_release( p_input );

    free( psz_title );
    free( psz_artist );
    free( psz_album );

    vlc_mutex_lock( &p_sys->lock );
    p_sys->b_queued = VLC_TRUE;
    p_sys->b_metadata_read = VLC_TRUE;
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}
