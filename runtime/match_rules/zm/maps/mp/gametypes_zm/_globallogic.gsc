// Script de règles central, Zombies.
// Réécrit en salle blanche, sans le script d'origine.
// En Zombies, _zm et _zm_gametype tiennent l'essentiel du jeu : ce script
// fournit le socle commun et ne se met pas en travers.

// ---------------------------------------------------------------------------
// Démarrage
// ---------------------------------------------------------------------------

init()
{
    // Constantes de type de session pour gamemodeismode().
    common_scripts\utility::init_session_mode_flags();

    level.gametype = tolower( getdvar( "g_gametype" ) );
    level.script = tolower( getdvar( "mapname" ) );

    set_environment_flags();

    level.teams = [];
    level.teams["allies"] = "allies";
    level.teams["axis"] = "axis";
    level.teamcount = 2;
    level.zombie_team = "axis";

    init_game_keys();
    init_strings();
    set_default_rules();
    init_level_state();

    // Lus avant que quiconque les écrive. La table des
    // groupes de dialogues est créée ici, avant le main() du mode qui en
    // inscrit, et n'est plus remise à zéro ensuite.
    level.dialoggroup = [];
    level.lastslowprocessframe = 0;
    level.laststatustime = 0;
    level.waswinning = [];

    // Compteurs incrémentés sans être créés en Zombies.
    level.objidstart = 0;
    level.globalshotsfired = 0;
    level.globalcrossbowfired = 0;
    level.globalkillstreaksdestroyed = 0;
}

set_environment_flags()
{
    level.splitscreen = 0;

    if ( issplitscreen() )
        level.splitscreen = 1;
    level.console = 0;
    level.xenon = 0;
    level.ps3 = 0;
    level.wiiu = 0;
    level.onlinegame = 0;
    level.rankedmatch = 0;
    level.systemlink = 0;
    level.leaguematch = 0;
    level.wagermatch = 0;
    level.multiteam = 0;

    level.hardcoremode = setting( "hardcoreMode", 0 );
    level.oldschool = getdvarint( "scr_oldschool" );
}

init_game_keys()
{
    if ( !isdefined( game["state"] ) )
        game["state"] = "playing";

    if ( !isdefined( game["roundsplayed"] ) )
        game["roundsplayed"] = 0;

    if ( !isdefined( game["timepassed"] ) )
        game["timepassed"] = 0;

    if ( !isdefined( game["roundMillisecondsAlreadyPassed"] ) )
        game["roundMillisecondsAlreadyPassed"] = 0;

    if ( !isdefined( game["switchedsides"] ) )
        game["switchedsides"] = 0;

    if ( !isdefined( game["attackers"] ) )
        game["attackers"] = "axis";

    if ( !isdefined( game["defenders"] ) )
        game["defenders"] = "allies";

    if ( !isdefined( game["roundswon"] ) )
        game["roundswon"] = [];

    if ( !isdefined( game["teamScores"] ) )
        game["teamScores"] = [];

    if ( !isdefined( game["roundswon"]["tie"] ) )
        game["roundswon"]["tie"] = 0;

    foreach ( team in level.teams )
    {
        if ( !isdefined( game["roundswon"][team] ) )
            game["roundswon"][team] = 0;

        if ( !isdefined( game["teamScores"][team] ) )
            game["teamScores"][team] = 0;
    }

    // Compteurs d'éliminations de toute la partie.
    if ( !isdefined( game["totalKills"] ) )
        game["totalKills"] = 0;

    if ( !isdefined( game["totalKillsTeam"] ) )
        game["totalKillsTeam"] = [];

    foreach ( team in level.teams )
    {
        if ( !isdefined( game["totalKillsTeam"][team] ) )
            game["totalKillsTeam"][team] = 0;
    }
}

init_strings()
{
    if ( !isdefined( game["strings"] ) )
        game["strings"] = [];

    // Clés vérifiées par l'équipe A.
    set_string( "press_to_spawn", &"PLATFORM_PRESS_TO_SPAWN" );
    set_string( "waiting_to_spawn", &"MP_WAITING_TO_SPAWN" );
    set_string( "waiting_to_spawn_ss", &"MP_WAITING_TO_SPAWN_SS" );
    set_string( "you_will_spawn", &"MP_YOU_WILL_RESPAWN" );
    set_string( "spawn_next_round", &"MP_SPAWN_NEXT_ROUND" );
    set_string( "waiting_for_teams", &"MP_WAITING_FOR_TEAMS" );
    set_string( "match_starting_in", &"MP_MATCH_STARTING_IN" );
    set_string( "opponent_forfeiting_in", &"MP_OPPONENT_FORFEITING_IN" );
    set_string( "players_forfeited", &"MP_PLAYERS_FORFEITED" );
    set_string( "tie", &"MP_MATCH_TIE" );
    set_string( "time_limit_reached", &"MP_TIME_LIMIT_REACHED" );
    set_string( "score_limit_reached", &"MP_SCORE_LIMIT_REACHED" );
}

// Un texte affiché doit être préchargé à chaque chargement de la carte, même
// quand game[] l'a gardé de la manche d'avant.
set_string( key, text )
{
    if ( !isdefined( game["strings"][key] ) )
        game["strings"][key] = text;

    precachestring( game["strings"][key] );
}

// N'appeler qu'avec un nom de réglage connu : un nom absent est une erreur
// moteur. Le repli ne couvre qu'une valeur indéfinie.
setting( name, fallback )
{
    value = getgametypesetting( name );

    if ( !isdefined( value ) )
        return fallback;

    return value;
}

// Défauts posés avant _zm_gametype, qui pose ensuite son profil Zombies
// Rien ici n'est réécrit au démarrage de la manche.
set_default_rules()
{
    level.timelimit = 0;
    level.scorelimit = 0;
    level.roundlimit = 1;
    level.roundwinlimit = 0;
    level.numlives = 0;
    level.roundswitch = 0;

    level.teambased = 0;
    level.endgameonscorelimit = 1;
    level.endgameontimelimit = 1;
    level.allowannouncer = 1;
    level.dontshowendreason = 0;
    level.dropteam = 0;

    level.playerrespawndelay = setting( "playerRespawnDelay", 0 );
    level.waverespawndelay = setting( "waveRespawnDelay", 0 );
    level.playerforcerespawn = setting( "playerForceRespawn", 0 );
    level.playerqueuedrespawn = setting( "playerQueuedRespawn", 0 );
    level.playermaxhealth = setting( "playerMaxHealth", 100 );
    level.playerhealthregentime = setting( "playerHealthRegenTime", 5 );
    level.playersprinttime = setting( "playerSprintTime", 4 );
    level.playerobjectiveheldrespawndelay = setting( "playerObjectiveHeldRespawnDelay", 0 );
    level.teamkillspawndelay = setting( "teamKillSpawnDelay", 0 );
    level.suicidespawndelay = 0;
    level.teamkilledspawndelay = 0;
    level.spawndelay = 0;
    level.livesdonotreset = 0;

    level.friendlyfiredelay = 0;
    level.friendlyfiredelaytime = 0;

    // Lus par les scripts du joueur sans que Zombies les pose.
    level.allowhitmarkers = setting( "allowHitMarkers", 0 );
    level.bulletdamagescalar = setting( "bulletDamageScalar", 1 );
    level.deathpointloss = setting( "deathPointLoss", 0 );
    level.teamkillpointloss = setting( "teamKillPointLoss", 0 );
    level.minimumallowedteamkills = setting( "teamKillPunishCount", 0 );
    level.onlyheadshots = setting( "onlyHeadshots", 0 );
    level.forceradar = setting( "forceRadar", 0 );
    level.spectatetype = setting( "spectateType", 0 );
    level.disablecac = setting( "disableCAC", 0 );

    level.disableweapondrop = 0;
    level.perksenabled = 1;
    level.inovertime = 0;
    level.usestartspawns = 1;
    level.killcam = 0;
    level.defaultclass = "CLASS_ASSAULT";
    level.globallarryskilled = 0;

    level.spawnmessage = maps\mp\gametypes_zm\_globallogic_spawn::default_spawnmessage;

    // Droits de parole lus entre deux manches : tout autorisé.
    level.voip = spawnstruct();
    level.voip.deadchatwithdead = 1;
    level.voip.deadchatwithteam = 1;
    level.voip.deadhearallliving = 1;
    level.voip.deadhearkiller = 1;
    level.voip.deadhearteamliving = 1;
    level.voip.everyonehearseveryone = 1;
    level.voip.killershearvictim = 1;
}

init_level_state()
{
    level.gameended = 0;
    level.forcedend = 0;
    level.hostforcedend = 0;
    level.ingraceperiod = 0;
    level.inprematchperiod = 0;
    level.prematchperiod = 0;
    level.timerstopped = 0;
    level.maxplayercount = 0;
    level.postroundtime = 0;
    level.infinalkillcam = 0;
    level.endgamesent = 0;
    level.timelimitreached = 0;
    level.matchendingsoonsent = 0;
    level.matchendingverysoonsent = 0;

    if ( !isdefined( level.players ) )
        level.players = [];

    level.placement = [];
    level.placement["all"] = [];

    level.playercount = [];
    level.alivecount = [];
    level.aliveplayers = [];
    level.deadplayers = [];
    level.everexisted = [];
    level.activeplayers = [];
    level.lastwave = [];
    level.wavedelay = [];
    level.waveplayerspawnindex = [];
    level.prevalivecount = [];
    level.deadeventsent = [];
    level.oneleftsent = [];

    foreach ( team in level.teams )
    {
        level.playercount[team] = 0;
        level.alivecount[team] = 0;
        level.aliveplayers[team] = [];
        level.deadplayers[team] = [];
        level.everexisted[team] = 0;
        level.lastwave[team] = 0;
        level.wavedelay[team] = 0;
        level.waveplayerspawnindex[team] = 0;
        level.prevalivecount[team] = 0;
        level.deadeventsent[team] = 0;
        level.oneleftsent[team] = 0;
    }
}

registerfriendlyfiredelay( gametype, defaultvalue, minvalue, maxvalue )
{
    value = defaultvalue;
    custom = getdvar( "scr_" + gametype + "_friendlyfiredelay" );

    if ( custom != "" )
        value = getdvarint( "scr_" + gametype + "_friendlyfiredelay" );

    if ( value < minvalue )
        value = minvalue;

    if ( value > maxvalue )
        value = maxvalue;

    level.friendlyfiredelaytime = value;
    level.friendlyfiredelay = value > 0;
}

callback_startgametype()
{
    game["state"] = "playing";
    init_game_keys();
    init_spawn_tables();
    precache_hud_assets();
    init_modules();

    [[ level.onprecachegametype ]]();
    [[ level.onstartgametype ]]();

    level.gameended = 0;
    level.ingraceperiod = 0;
    level.inprematchperiod = 0;
    level.placement["all"] = [];
    level.activeplayers = [];

    if ( !isdefined( level.players ) )
        level.players = [];

    level thread maps\mp\gametypes_zm\_globallogic_utils::gametimer();
    level thread watch_time_limit();
    level thread watch_end_game();

    if ( level.waverespawndelay > 0 && isdefined( level.wavespawntimer ) )
        level thread [[ level.wavespawntimer ]]();

    level thread announce_prematch_over();
}

// Tables des points d'apparition, que _spawning lit avant d'y écrire ; « free »
// sert quand la partie n'est pas par équipes.
init_spawn_tables()
{
    level.spawn_point_team_class_names = [];
    level.teamspawnpoints = [];
    foreach ( team in level.teams )
    {
        level.spawn_point_team_class_names[team] = [];
        level.teamspawnpoints[team] = [];
    }

    level.spawn_point_team_class_names["free"] = [];
    level.teamspawnpoints["free"] = [];
}

// Icônes d'état du tableau des scores (étendue aux Zombies
// par Q41) et image des notifications à l'écran.
precache_hud_assets()
{
    precachestatusicon( "hud_status_connecting" );
    precachestatusicon( "hud_status_dead" );
    precacheshader( "white" );
}

// Avec _weaponobjects à la place de _weapons, et sans _menus :
// il ne se lie pas et _zm_gametype traite déjà les menus.
// Les modules qui attendent partent en fil séparé pour ne pas bloquer le
// démarrage.
init_modules()
{
    maps\mp\_ballistic_knife::init();
    maps\mp\_bb::init();
    level thread maps\mp\_challenges::init();

    maps\mp\gametypes_zm\_clientids::init();
    maps\mp\gametypes_zm\_damagefeedback::init();
    maps\mp\gametypes_zm\_gameobjects::init();
    maps\mp\gametypes_zm\_globallogic_audio::init();
    maps\mp\gametypes_zm\_globallogic_ui::init();
    maps\mp\gametypes_zm\_healthoverlay::init();
    maps\mp\gametypes_zm\_hud::init();
    maps\mp\gametypes_zm\_hud_message::init();
    maps\mp\gametypes_zm\_scoreboard::init();
    level thread maps\mp\gametypes_zm\_serversettings::init();
    maps\mp\gametypes_zm\_shellshock::init();
    maps\mp\gametypes_zm\_spawning::init();
    maps\mp\gametypes_zm\_spawnlogic::init();
    maps\mp\gametypes_zm\_spectating::init();
    maps\mp\gametypes_zm\_tweakables::init();
    maps\mp\gametypes_zm\_weaponobjects::init();
}

// Pas d'avant-match en Zombies : prematch_over part tout de suite.
announce_prematch_over()
{
    // Laisse aux fils lancés pendant le montage le temps d'attendre.
    waittillframeend;
    level notify( "prematch_over" );
}

// Note si _zm a déjà lancé son Game Over, pour ne pas le déclencher deux fois.
watch_end_game()
{
    level waittill( "end_game" );
    level.endgamesent = 1;
}

// ---------------------------------------------------------------------------
// Temps : la limite peut être posée en cours de partie (zcleansed).
// ---------------------------------------------------------------------------

round_elapsed_ms()
{
    if ( !isdefined( level.starttime ) )
        return 0;

    now = gettime();

    if ( level.timerstopped && isdefined( level.timerpausetime ) )
        now = level.timerpausetime;

    discard = 0;

    if ( isdefined( level.discardtime ) )
        discard = level.discardtime;

    elapsed = now - level.starttime - discard;

    if ( elapsed < 0 )
        return 0;

    return elapsed;
}

watch_time_limit()
{
    level endon( "game_ended" );

    level.clocklimit = 0;
    level.clockpaused = 1;

    for ( ;; )
    {
        wait 0.25;

        if ( !isdefined( level.starttime ) || level.timelimitreached )
            continue;

        limit = 0;

        if ( isdefined( level.timelimit ) )
            limit = level.timelimit;

        remaining = int( limit * 60000 ) - round_elapsed_ms();
        update_clock( limit, remaining );

        if ( limit <= 0 )
            continue;

        check_match_ending_soon( remaining );

        if ( remaining > 0 )
            continue;

        level.timelimitreached = 1;
        [[ level.ontimelimit ]]();
    }
}

// Horloge de manche. Réglée seulement quand l'état change :
// début du jeu effectif, reprise après une pause, limite modifiée, limite
// retirée. Jamais pendant une pause : sd, dem, zcleansed et _hostmigration
// gèrent alors eux-mêmes l'affichage.
update_clock( limit, remaining )
{
    if ( limit <= 0 )
    {
        if ( level.clocklimit > 0 )
            setgameendtime( 0 );
    }
    else if ( !level.timerstopped && ( level.clockpaused || limit != level.clocklimit ) )
    {
        setgameendtime( gettime() + remaining );
    }

    level.clocklimit = limit;
    level.clockpaused = level.timerstopped;
}

// Une seule partie de règles en Zombies : very_soon plutôt que vox.
check_match_ending_soon( remaining )
{
    if ( level.gameended || remaining <= 0 )
        return;

    if ( !level.matchendingsoonsent && remaining <= 60000 )
    {
        level.matchendingsoonsent = 1;
        level notify( "match_ending_soon" );
    }

    if ( !level.matchendingverysoonsent && remaining <= 30000 )
    {
        level.matchendingverysoonsent = 1;
        level notify( "match_ending_very_soon" );
    }
}

// ---------------------------------------------------------------------------
// Limites de score (aucune en Zombies standard)
// ---------------------------------------------------------------------------

checkscorelimit()
{
    if ( level.gameended || level.scorelimit <= 0 )
        return;

    if ( level.teambased )
    {
        foreach ( team in level.teams )
        {
            if ( game["teamScores"][team] >= level.scorelimit )
            {
                [[ level.onscorelimit ]]();
                return;
            }
        }

        return;
    }

    if ( isplayer( self ) && isdefined( self.pointstowin ) && self.pointstowin >= level.scorelimit )
        [[ level.onscorelimit ]]();
}

checkteamscorelimitsoon( team )
{
    if ( level.gameended || level.scorelimit <= 0 || !level.endgameonscorelimit )
        return;

    score = game["teamScores"][team];
    elapsed = round_elapsed_ms();

    if ( !isdefined( score ) || score <= 0 || elapsed <= 0 )
        return;

    // Projection linéaire au rythme actuel de l'équipe.
    check_match_ending_soon( int( ( level.scorelimit - score ) * elapsed / score ) );
}

// ---------------------------------------------------------------------------
// Effectifs
// ---------------------------------------------------------------------------

updateteamstatus()
{
    level.activeplayers = [];
    lives = [];

    foreach ( team in level.teams )
    {
        level.playercount[team] = 0;
        level.alivecount[team] = 0;
        level.aliveplayers[team] = [];
        level.deadplayers[team] = [];
        lives[team] = 0;
    }

    total = 0;

    foreach ( player in level.players )
    {
        if ( !is_active_player( player ) )
            continue;

        total++;
        alive = is_player_alive( player );

        if ( alive )
            level.activeplayers[level.activeplayers.size] = player;

        team = player.team;

        if ( !isdefined( level.teams[team] ) )
            continue;

        level.playercount[team]++;
        level.everexisted[team] = 1;

        if ( alive )
        {
            level.alivecount[team]++;
            level.aliveplayers[team][level.aliveplayers[team].size] = player;
        }
        else
        {
            level.deadplayers[team][level.deadplayers[team].size] = player;

            if ( has_life_left( player ) )
                lives[team]++;
        }
    }

    if ( total > level.maxplayercount )
        level.maxplayercount = total;

    level.teamliveswaiting = lives;
    level thread dispatch_team_events();
}

is_active_player( player )
{
    return isdefined( player ) && isdefined( player.team ) && player.team != "spectator";
}

is_player_alive( player )
{
    return isalive( player ) && isdefined( player.sessionstate ) && player.sessionstate == "playing";
}

// pers["lives"] est posé à la connexion. Indéfini = il reste
// une vie : un joueur qui n'est pas encore apparu ne compte pas comme mort.
has_life_left( player )
{
    // Vies illimitées : toujours une vie.
    if ( level.numlives == 0 )
        return 1;

    if ( !isdefined( player.pers ) || !isdefined( player.pers["lives"] ) )
        return 1;

    return player.pers["lives"] > 0;
}

// Crochets une fois par image, sur l'état final. _zm_gametype ne pose ni
// onalivecountchange ni onlastteamaliveevent : ils sont appelés avec garde.
dispatch_team_events()
{
    level notify( "dispatch_team_events" );
    level endon( "dispatch_team_events" );
    level endon( "game_ended" );

    waittillframeend;

    if ( level.gameended || level.ingraceperiod )
        return;

    foreach ( team in level.teams )
    {
        if ( level.alivecount[team] != level.prevalivecount[team] )
        {
            level.prevalivecount[team] = level.alivecount[team];

            if ( isdefined( level.onalivecountchange ) )
                [[ level.onalivecountchange ]]( team );
        }
    }

    if ( level.numlives == 0 )
        return;

    // Une équipe qui n'a jamais eu de joueur (les zombies sont des acteurs) ne
    // meurt pas : seule l'équipe humaine déclenche l'événement.
    dead = [];

    foreach ( team in level.teams )
    {
        isdead = level.everexisted[team] && level.playercount[team] > 0 && level.alivecount[team] == 0 && level.teamliveswaiting[team] == 0;

        if ( !isdead )
            level.deadeventsent[team] = 0;
        else if ( !level.deadeventsent[team] )
            dead[dead.size] = team;
    }

    if ( dead.size > 0 )
    {
        foreach ( team in dead )
            level.deadeventsent[team] = 1;

        if ( dead.size == level.teams.size )
        {
            [[ level.ondeadevent ]]( "all" );
            return;
        }

        foreach ( team in dead )
            [[ level.ondeadevent ]]( team );
    }

    if ( level.gameended || !isdefined( level.ononeleftevent ) )
        return;

    foreach ( team in level.teams )
    {
        if ( level.alivecount[team] != 1 || level.playercount[team] < 2 )
        {
            level.oneleftsent[team] = 0;
            continue;
        }

        if ( !level.oneleftsent[team] )
        {
            level.oneleftsent[team] = 1;
            [[ level.ononeleftevent ]]( team );
        }
    }
}

// ---------------------------------------------------------------------------
// Classement
// ---------------------------------------------------------------------------

updateplacement()
{
    ranked = [];

    foreach ( player in level.players )
    {
        if ( is_active_player( player ) )
            ranked[ranked.size] = player;
    }

    for ( i = 1; i < ranked.size; i++ )
    {
        current = ranked[i];
        j = i - 1;

        while ( j >= 0 && ranks_before( current, ranked[j] ) )
        {
            ranked[j + 1] = ranked[j];
            j--;
        }

        ranked[j + 1] = current;
    }

    level.placement["all"] = ranked;

    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player notify( "update_outcome" );
    }
}

ranks_before( a, b )
{
    if ( !level.teambased )
    {
        pa = score_of( a.pointstowin );
        pb = score_of( b.pointstowin );

        if ( pa != pb )
            return pa > pb;
    }

    return score_of( a.score ) > score_of( b.score );
}

score_of( value )
{
    if ( !isdefined( value ) )
        return 0;

    return value;
}

removedisconnectedplayerfromplacement()
{
    kept = [];

    foreach ( player in level.placement["all"] )
    {
        if ( isdefined( player ) && isplayer( player ) )
            kept[kept.size] = player;
    }

    level.placement["all"] = kept;
    updateplacement();
}

determineteamwinnerbygamestat( stat )
{
    winner = "tie";
    best = undefined;

    foreach ( team in level.teams )
    {
        value = game[stat][team];

        if ( !isdefined( best ) || value > best )
        {
            best = value;
            winner = team;
        }
        else if ( value == best )
        {
            winner = "tie";
        }
    }

    return winner;
}

// ---------------------------------------------------------------------------
// Fin de partie
// ---------------------------------------------------------------------------

forceend()
{
    if ( level.gameended || level.forcedend )
        return;

    level.forcedend = 1;
    level.hostforcedend = 1;
    endgame( determineteamwinnerbygamestat( "teamScores" ), "" );
}

killserverpc()
{
    forceend();
}

listenforgameend()
{
    self endon( "disconnect" );
    level waittill( "game_ended" );
}

endgame( winner, endreasontext )
{
    if ( level.gameended )
        return;

    level.gameended = 1;

    // Suite dans un fil à part : l'appelant porte souvent un endon( "game_ended" ).
    level thread run_endgame( winner, endreasontext );
}

run_endgame( winner, endreasontext )
{
    if ( !isdefined( endreasontext ) )
        endreasontext = "";

    level.gameendtime = gettime();
    game["state"] = "postgame";

    if ( !level.timerstopped )
    {
        level.timerstopped = 1;
        level.timerpausetime = gettime();
    }

    game["timepassed"] += round_elapsed_ms();
    setgameendtime( 0 );
    level notify( "game_ended" );

    game["roundsplayed"]++;

    if ( isdefined( winner ) && !isplayer( winner ) && ( winner == "tie" || isdefined( level.teams[winner] ) ) )
        game["roundswon"][winner]++;

    if ( isdefined( level.onendgame ) )
        [[ level.onendgame ]]( winner );

    final = winner;

    if ( isdefined( level.onroundendgame ) )
    {
        result = [[ level.onroundendgame ]]( winner );

        if ( isdefined( result ) )
            final = result;
    }

    // Si _zm a déjà commencé son Game Over, on s'en tient à la comptabilité.
    if ( game_over_started() )
        return;

    show_outcome( final, endreasontext );
    maps\mp\gametypes_zm\_globallogic_score::updatewinlossstats( final );

    // Grief émet lui-même end_game après son tableau ; seule une fin forcée
    // le fait ici.
    if ( level.gametype == "zgrief" && !level.forcedend )
        return;

    // Laisser lire le résultat, puis l'effacer pour qu'il ne recouvre pas le
    // « GAME OVER » de _zm.
    wait 3;

    if ( game_over_started() )
        return;

    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player notify( "reset_outcome" );
    }

    level.endgamesent = 1;
    level notify( "end_game" );
}

game_over_started()
{
    if ( level.endgamesent )
        return 1;

    return isdefined( level.intermission ) && level.intermission;
}

show_outcome( winner, endreasontext )
{
    if ( !isdefined( level.onteamoutcomenotify ) )
        return;

    if ( level.dontshowendreason )
        endreasontext = "";

    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player thread [[ level.onteamoutcomenotify ]]( winner, 0, endreasontext );
    }
}

// ---------------------------------------------------------------------------
// Réapparition par vagues. Inactive en Zombies standard.
// ---------------------------------------------------------------------------

// _globallogic_spawn fait apparaître les joueurs d'après lastwave et wavedelay :
// ici, on ne fait que marquer chaque vague.
wavespawntimer()
{
    level endon( "game_ended" );

    if ( level.waverespawndelay <= 0 )
        return;

    foreach ( team in level.teams )
        level.wavedelay[team] = level.waverespawndelay;

    for ( ;; )
    {
        foreach ( team in level.teams )
        {
            level.lastwave[team] = gettime();
            level.waveplayerspawnindex[team] = 0;
        }

        wait level.waverespawndelay;
    }
}

// ---------------------------------------------------------------------------
// Statistiques et historique. Hors ligne : sans effet.
// ---------------------------------------------------------------------------

getcurrentgamemode()
{
    if ( level.leaguematch )
        return "leaguematch";

    if ( level.rankedmatch )
        return "publicmatch";

    return "privatematch";
}

incrementmatchcompletionstat( gamemode, playedorhosted, stat )
{
    if ( !level.onlinegame )
        return;
}

getgamelength()
{
    if ( level.gameended )
        return int( game["timepassed"] / 1000 );

    return int( ( game["timepassed"] + round_elapsed_ms() ) / 1000 );
}

bbplayermatchend( gamelength, endreasonstring, gameover )
{
}

getkillstreaks( player )
{
    return [];
}

gamehistoryplayerquit()
{
    bbplayermatchend( getgamelength(), "MP_PLAYER_QUIT", 0 );
}

// Défaut de crochet, comparé par identité.
blank()
{
}
