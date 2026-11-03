// Script de règles central, multijoueur.
// Réécrit en salle blanche, sans le script d'origine.

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
    setup_teams();
    init_game_keys();
    init_strings();
    set_default_rules();
    init_level_state();

    level.infinalkillcam = 0;
    level.usestartspawns = 1;

    // Lus avant que quiconque les écrive. La table des
    // groupes de dialogues est créée ici, avant le main() du mode qui en
    // inscrit, et n'est plus remise à zéro ensuite.
    level.dialoggroup = [];
    level.lastslowprocessframe = 0;
    level.laststatustime = 0;
    level.waswinning = [];
    level.onplayerkilledextraunthreadedcbs = [];

    // Rayon d'objectif commun aux modes et compteur d'objets transportables
    // de _gameobjects.
    level.defaultoffenseradius = 300;
    level.objidstart = 0;
}

set_environment_flags()
{
    // Portage hors ligne : partie privée locale.
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

    level.hardcoremode = setting( "hardcoreMode", 0 );
    level.oldschool = getdvarint( "scr_oldschool" );

    level.multiteam = 0;
    level.teamcount = 2;
}

setup_teams()
{
    level.teams = [];
    level.teams["allies"] = "allies";
    level.teams["axis"] = "axis";

    if ( level.multiteam )
    {
        for ( i = 3; i <= 8; i++ )
        {
            name = "team" + i;
            level.teams[name] = name;
        }
    }

    level.teamcount = level.teams.size;
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

    if ( !isdefined( game["roundswon"] ) )
        game["roundswon"] = [];

    if ( !isdefined( game["teamScores"] ) )
        game["teamScores"] = [];

    if ( !isdefined( game["lastroundscore"] ) )
        game["lastroundscore"] = [];

    if ( !isdefined( game["roundswon"]["tie"] ) )
        game["roundswon"]["tie"] = 0;

    foreach ( team in level.teams )
    {
        if ( !isdefined( game["roundswon"][team] ) )
            game["roundswon"][team] = 0;

        if ( !isdefined( game["teamScores"][team] ) )
            game["teamScores"][team] = 0;

        if ( !isdefined( game["lastroundscore"][team] ) )
            game["lastroundscore"][team] = 0;
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

    // Clés vérifiées par l'équipe A. Posées avant le mode,
    // qui peut en remplacer (ctf, oneflag : score_limit_reached).
    set_string( "press_to_spawn", &"PLATFORM_PRESS_TO_SPAWN" );
    set_string( "waiting_to_spawn", &"MP_WAITING_TO_SPAWN" );
    set_string( "waiting_to_spawn_ss", &"MP_WAITING_TO_SPAWN_SS" );
    set_string( "you_will_spawn", &"MP_YOU_WILL_RESPAWN" );
    set_string( "spawn_next_round", &"MP_SPAWN_NEXT_ROUND" );
    set_string( "waiting_for_teams", &"MP_WAITING_FOR_TEAMS" );
    set_string( "match_starting_in", &"MP_MATCH_STARTING_IN" );
    set_string( "opponent_forfeiting_in", &"MP_OPPONENT_FORFEITING_IN" );
    set_string( "players_forfeited", &"MP_PLAYERS_FORFEITED" );
    set_string( "other_teams_forfeited", &"MP_OTHER_TEAMS_FORFEITED" );
    set_string( "tie", &"MP_MATCH_TIE" );
    set_string( "time_limit_reached", &"MP_TIME_LIMIT_REACHED" );
    set_string( "score_limit_reached", &"MP_SCORE_LIMIT_REACHED" );
    set_string( "enemies_eliminated", &"MP_ENEMIES_ELIMINATED" );
    set_string( "change_class", &"MP_CHANGE_CLASS_NEXT_SPAWN" );
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

// Valeurs par défaut des réglages. Le main() du mode passe
// après init() et peut les remplacer.
set_default_rules()
{
    level.timelimit = 0;
    level.scorelimit = 0;
    level.roundlimit = 1;
    level.roundwinlimit = 0;
    level.numlives = 0;
    level.roundswitch = 0;

    level.teambased = 0;
    level.overrideteamscore = 0;
    level.overrideplayerscore = 0;
    level.scoreroundbased = 0;
    level.resetplayerscoreeveryround = 0;
    level.endgameonscorelimit = 1;
    level.endgameontimelimit = 1;
    level.displayhalftimetext = 0;
    level.displayroundendtext = 1;
    level.allowannouncer = setting( "allowAnnouncer", 1 );
    level.disableprematchmessages = 0;
    level.disablemomentum = 0;
    level.disablechallenges = 0;
    level.disablestattracking = 0;
    level.dontshowendreason = 0;
    level.nopersistence = 0;
    level.forceautoassign = 0;
    level.forceallallies = 0;
    level.disableclassselection = setting( "disableClassSelection", 0 );
    level.dropteam = 0;
    level.forceradar = setting( "forceRadar", 0 );
    level.playersprinttime = setting( "playerSprintTime", 4 );

    level.playerrespawndelay = setting( "playerRespawnDelay", 0 );
    level.waverespawndelay = setting( "waveRespawnDelay", 0 );
    level.playerforcerespawn = setting( "playerForceRespawn", 0 );
    level.playerqueuedrespawn = setting( "playerQueuedRespawn", 0 );
    level.livesdonotreset = 0;
    level.playermaxhealth = setting( "playerMaxHealth", 100 );
    level.playerhealthregentime = setting( "playerHealthRegenTime", 5 );
    level.playerobjectiveheldrespawndelay = setting( "playerObjectiveHeldRespawnDelay", 0 );
    level.suicidespawndelay = 0;
    level.teamkilledspawndelay = 0;
    level.teamkillspawndelay = setting( "teamKillSpawnDelay", 0 );
    level.spawndelay = 0;

    level.friendlyfiredelay = 0;
    level.friendlyfiredelaytime = 0;
    level.minimumallowedteamkills = setting( "teamKillPunishCount", 0 );
    level.maxsuicidesbeforekick = setting( "maxSuicidesBeforeKick", 0 );
    level.teamkillpointloss = setting( "teamKillPointLoss", 0 );
    level.teamkillreducedpenalty = setting( "teamKillReducedPenalty", 0 );
    level.deathpointloss = setting( "deathPointLoss", 0 );

    level.bulletdamagescalar = setting( "bulletDamageScalar", 1 );
    level.vehicledamagescalar = 1;
    level.onlyheadshots = setting( "onlyHeadshots", 0 );
    level.perksenabled = 1;
    level.attachmentsdisabled = setting( "disableAttachments", 0 );
    level.disabletacinsert = setting( "disableTacInsert", 0 );
    level.disableweapondrop = 0;
    level.allowhitmarkers = setting( "allowHitMarkers", 0 );
    level.rankcap = 0;
    level.minprestige = 0;

    level.squads = 0;
    level.spectatetype = setting( "spectateType", 0 );
    level.roundscorecarry = setting( "roundscorecarry", 0 );
}

// État de la manche. Appelée par init() et au montage.
init_level_state()
{
    level.gameended = 0;
    level.forcedend = 0;
    level.hostforcedend = 0;
    level.ingraceperiod = 0;
    level.inprematchperiod = 0;
    level.prematchperiod = 0;
    level.inovertime = 0;
    level.intermission = 0;
    level.timerstopped = 0;
    level.maxplayercount = 0;
    level.postroundtime = 0;

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
    level.lastteamalivesent = 0;
    level.ffaoneleftsent = 0;
    level.forfeitpending = 0;
    level.matchendingsoonsent = 0;
    level.matchendingverysoonsent = 0;
    level.timelimitreached = 0;

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

// Seulement les champs listés, pour ne rien écraser de ce que
// onstartgametype vient de poser.
reset_round_state()
{
    level.gameended = 0;
    level.infinalkillcam = 0;
    level.timerstopped = 0;
    level.dropteam = 0;
    level.maxplayercount = 0;
    level.placement["all"] = [];
    level.activeplayers = [];

    if ( !isdefined( level.players ) )
        level.players = [];

    foreach ( team in level.teams )
    {
        level.playercount[team] = 0;
        level.alivecount[team] = 0;
        level.aliveplayers[team] = [];
        level.deadplayers[team] = [];
    }
}

// Crochets par défaut. Le mode remplace ceux qu'il veut.
setupcallbacks()
{
    level.spawnplayer = maps\mp\gametypes\_globallogic_spawn::spawnplayer;
    level.spawnplayerprediction = maps\mp\gametypes\_globallogic_spawn::spawnplayerprediction;
    level.spawnclient = maps\mp\gametypes\_globallogic_spawn::spawnclient;
    level.spawnspectator = maps\mp\gametypes\_globallogic_spawn::spawnspectator;
    level.spawnintermission = maps\mp\gametypes\_globallogic_spawn::spawnintermission;
    level.spawnmessage = maps\mp\gametypes\_globallogic_spawn::default_spawnmessage;

    level.onspawnspectator = maps\mp\gametypes\_globallogic_defaults::default_onspawnspectator;
    level.onspawnintermission = maps\mp\gametypes\_globallogic_defaults::default_onspawnintermission;

    level.onplayerscore = maps\mp\gametypes\_globallogic_score::default_onplayerscore;
    level.onteamscore = maps\mp\gametypes\_globallogic_score::default_onteamscore;
    level.iskillboosting = maps\mp\gametypes\_globallogic_score::default_iskillboosting;
    level._setteamscore = maps\mp\gametypes\_globallogic_score::_setteamscore;
    level._getteamscore = maps\mp\gametypes\_globallogic_score::_getteamscore;
    level._setplayerscore = maps\mp\gametypes\_globallogic_score::_setplayerscore;
    level._getplayerscore = maps\mp\gametypes\_globallogic_score::_getplayerscore;

    level.onplayerdisconnect = ::blank;
    level.onplayerdamage = ::blank;
    level.onplayerkilled = ::blank;
    level.onrespawndelay = ::blank;
    level.gettimelimit = maps\mp\gametypes\_globallogic_defaults::default_gettimelimit;
    level.getteamkillpenalty = maps\mp\gametypes\_globallogic_defaults::default_getteamkillpenalty;
    level.getteamkillscore = maps\mp\gametypes\_globallogic_defaults::default_getteamkillscore;

    level.onstartgametype = ::blank;
    level.onprecachegametype = ::blank;
    level.onroundswitch = ::blank;
    level.onendgame = ::blank;
    level.onroundendgame = maps\mp\gametypes\_globallogic_defaults::default_onroundendgame;

    level.ontimelimit = maps\mp\gametypes\_globallogic_defaults::default_ontimelimit;
    level.onscorelimit = maps\mp\gametypes\_globallogic_defaults::default_onscorelimit;
    level.onforfeit = maps\mp\gametypes\_globallogic_defaults::default_onforfeit;
    level.ondeadevent = maps\mp\gametypes\_globallogic_defaults::default_ondeadevent;
    level.ononeleftevent = maps\mp\gametypes\_globallogic_defaults::default_ononeleftevent;
    level.onlastteamaliveevent = maps\mp\gametypes\_globallogic_defaults::default_onlastteamaliveevent;
    level.onalivecountchange = maps\mp\gametypes\_globallogic_defaults::default_onalivecountchange;

    level.onteamoutcomenotify = maps\mp\gametypes\_hud_message::teamoutcomenotify;
    level.onoutcomenotify = maps\mp\gametypes\_hud_message::outcomenotify;
    level.onteamwageroutcomenotify = maps\mp\gametypes\_hud_message::teamwageroutcomenotify;
    level.onwageroutcomenotify = maps\mp\gametypes\_hud_message::wageroutcomenotify;
    level.setmatchscorehudelemforteam = maps\mp\gametypes\_hud_message::setmatchscorehudelemforteam;

    level.onmedalawarded = ::blank;
    level.onplayerconnect = ::blank;

    // Noms confirmés par l'équipe A. Ni level.allies ni
    // level.axis : aucun script ne les lit.
    level.autoassign = maps\mp\gametypes\_globallogic_ui::menuautoassign;
    level.spectator = maps\mp\gametypes\_globallogic_ui::menuspectator;
    level.class = maps\mp\gametypes\_globallogic_ui::menuclass;
    level.teammenu = maps\mp\gametypes\_globallogic_ui::menuteam;
}

registerfriendlyfiredelay( gametype, defaultvalue, minvalue, maxvalue )
{
    // Nom du réglage inconnu : dvar facultatif, sinon le défaut.
    value = defaultvalue;
    custom = getdvar( "scr_" + gametype + "_friendlyfiredelay" );

    if ( custom != "" )
        value = getdvarint( "scr_" + gametype + "_friendlyfiredelay" );

    value = maps\mp\gametypes\_globallogic_utils::getvalueinrange( value, minvalue, maxvalue );
    level.friendlyfiredelaytime = value;
    level.friendlyfiredelay = value > 0;
}

callback_startgametype()
{
    // Jamais « postgame » au début d'une manche. Le salon d'avant-partie
    // garde son « pregame ».
    if ( !( is_pregame() && game["state"] == "pregame" ) )
        game["state"] = "playing";

    init_game_keys();
    set_sides();
    init_strings();
    precache_team_strings();
    init_spawn_tables();
    init_modules();

    [[ level.onprecachegametype ]]();
    [[ level.onstartgametype ]]();

    reset_round_state();
    level.ingraceperiod = 1;

    restore_team_scores();

    level thread maps\mp\gametypes\_globallogic_utils::gametimer();
    level thread maps\mp\gametypes\_globallogic_audio::musiccontroller();
    level thread watch_time_limit();
    level thread show_waiting_for_teams();
    // Fil d'écoute de la killcam finale, que la fin de partie attend
    //.
    level thread maps\mp\gametypes\_killcam::dofinalkillcam();

    if ( level.waverespawndelay > 0 )
        level thread wave_spawn_timer();

    start_prematch();
}

is_pregame()
{
    return isdefined( level.pregame ) && level.pregame;
}

// Le moteur charge lui-même le jeu d'équipes de chaque faction de la carte : on
// n'en référence aucun. Base fixe des rôles à chaque manche
// ; les modes inversent eux-mêmes les camps quand game["switchedsides"]
// est vrai.
set_sides()
{
    game["attackers"] = "allies";
    game["defenders"] = "axis";
}

// Textes rangés dans game["strings"] par le script de chaque équipe, chargé
// avant le démarrage du mode.
precache_team_strings()
{
    suffixes = [];
    suffixes[suffixes.size] = "_name";
    suffixes[suffixes.size] = "_win";
    suffixes[suffixes.size] = "_win_round";
    suffixes[suffixes.size] = "_mission_accomplished";
    suffixes[suffixes.size] = "_eliminated";
    suffixes[suffixes.size] = "_forfeited";

    foreach ( team in level.teams )
    {
        foreach ( suffix in suffixes )
        {
            if ( isdefined( game["strings"][team + suffix] ) )
                precachestring( game["strings"][team + suffix] );
        }
    }
}

// Tables des points d'apparition, que _spawnlogic lit avant d'y écrire
//.
init_spawn_tables()
{
    level.spawn_point_team_class_names = [];
    level.teamspawnpoints = [];
    foreach ( team in level.teams )
    {
        level.spawn_point_team_class_names[team] = [];
        level.teamspawnpoints[team] = [];
    }
}

// Sans _teargrenades ni _copter, qui ne se lient pas
//, ni _tabun, qui précharge des armes absentes des zones.
// Les modules qui attendent partent en fil séparé pour ne pas bloquer le
// démarrage.
init_modules()
{
    maps\mp\_bb::init();
    maps\mp\_decoy::init();
    maps\mp\_gameadvertisement::init();
    maps\mp\_gamerep::init();
    maps\mp\_menus::init();
    maps\mp\_multi_extracam::init();
    maps\mp\_mgturret::main();

    maps\mp\gametypes\_battlechatter_mp::init();
    maps\mp\gametypes\_clientids::init();
    maps\mp\gametypes\_damagefeedback::init();
    maps\mp\gametypes\_deathicons::init();
    maps\mp\gametypes\_dev::init();
    level thread maps\mp\gametypes\_friendicons::init();
    maps\mp\gametypes\_gameobjects::init();
    maps\mp\gametypes\_globallogic_audio::init();
    maps\mp\gametypes\_globallogic_ui::init();
    maps\mp\gametypes\_healthoverlay::init();
    maps\mp\gametypes\_hud::init();
    maps\mp\gametypes\_hud_message::init();
    maps\mp\gametypes\_killcam::init();
    maps\mp\gametypes\_menus::init();
    maps\mp\gametypes\_objpoints::init();
    maps\mp\gametypes\_persistence::init();
    maps\mp\gametypes\_scoreboard::init();
    level thread maps\mp\gametypes\_serversettings::init();
    maps\mp\gametypes\_shellshock::init();
    maps\mp\gametypes\_spawnlogic::init();
    maps\mp\gametypes\_spectating::init();
    maps\mp\gametypes\_wager::init();
    maps\mp\gametypes\_weapons::init();
    maps\mp\gametypes\_class::initperkdvars();
    maps\mp\_burnplayer::initburnplayer();

    level thread maps\mp\teams\_teams::init();
    level thread maps\mp\bots\_bot::init();
    maps\mp\killstreaks\_killstreaks::init();

    // Icônes du tableau des scores posées par les scripts du joueur sans
    // préchargement de leur part.
    precachestatusicon( "hud_status_connecting" );
    precachestatusicon( "hud_status_dead" );
}

restore_team_scores()
{
    foreach ( team in level.teams )
    {
        if ( level.scoreroundbased )
            [[ level._setteamscore ]]( team, 0 );
        else
            [[ level._setteamscore ]]( team, game["teamScores"][team] );
    }
}

// ---------------------------------------------------------------------------
// Avant-match et période de grâce
// ---------------------------------------------------------------------------

start_prematch()
{
    duration = 0;

    if ( !isdefined( level.doprematch ) || level.doprematch )
    {
        if ( game["roundsplayed"] == 0 )
            duration = setting( "prematchperiod", 15 );
        else
            duration = setting( "preroundperiod", 5 );
    }

    level.prematchperiod = duration;
    level.inprematchperiod = duration > 0;
    level thread prematch( duration );
}

prematch( duration )
{
    level endon( "game_ended" );

    // Laisse aux fils lancés pendant le montage le temps d'attendre prematch_over.
    waittillframeend;

    if ( duration > 0 )
    {
        if ( level.wagermatch )
            level thread maps\mp\gametypes\_wager::prematchperiod();

        if ( !level.disableprematchmessages )
            level thread show_prematch_countdown( duration );

        wait duration;

        level.inprematchperiod = 0;

        foreach ( player in level.players )
        {
            if ( isdefined( player ) )
                player freezecontrols( 0 );
        }
    }

    level notify( "prematch_over" );
    grace_period();
}

show_prematch_countdown( duration )
{
    label = newhudelem();
    label.alignx = "center";
    label.aligny = "middle";
    label.horzalign = "center";
    label.vertalign = "middle";
    label.y = -60;
    label.fontscale = 1.5;
    label.foreground = 1;
    label.hidewheninmenu = 1;
    label settext( game["strings"]["match_starting_in"] );

    timer = newhudelem();
    timer.alignx = "center";
    timer.aligny = "middle";
    timer.horzalign = "center";
    timer.vertalign = "middle";
    timer.y = -35;
    timer.fontscale = 2;
    timer.foreground = 1;
    timer.hidewheninmenu = 1;
    timer settimer( duration );

    endtime = gettime() + int( duration * 1000 );

    while ( gettime() < endtime && !level.gameended )
        wait 0.05;

    label destroy();
    timer destroy();
}

grace_period()
{
    level endon( "game_ended" );

    wait setting( "graceperiod", 15 );

    level.ingraceperiod = 0;
    level.usestartspawns = 0;
    updateteamstatus();
}

show_waiting_for_teams()
{
    if ( !level.teambased || is_pregame() )
        return;

    if ( all_teams_existed() )
        return;

    text = newhudelem();
    text.alignx = "center";
    text.aligny = "middle";
    text.horzalign = "center";
    text.vertalign = "middle";
    text.y = -90;
    text.fontscale = 1.5;
    text.foreground = 1;
    text.hidewheninmenu = 1;
    text settext( game["strings"]["waiting_for_teams"] );

    while ( !level.gameended && !all_teams_existed() )
        wait 0.5;

    text destroy();
}

all_teams_existed()
{
    foreach ( team in level.teams )
    {
        if ( !level.everexisted[team] )
            return 0;
    }

    return 1;
}

// ---------------------------------------------------------------------------
// Temps
// ---------------------------------------------------------------------------

// Temps de jeu effectif de la manche, en ms, d'après l'état tenu par gametimer.
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

        if ( !isdefined( level.starttime ) )
            continue;

        limit = [[ level.gettimelimit ]]();

        if ( !isdefined( limit ) )
            limit = 0;

        remaining = int( limit * 60000 ) - round_elapsed_ms();
        update_clock( limit, remaining );

        if ( limit <= 0 )
            continue;

        check_match_ending_soon( remaining );

        if ( remaining > 0 || level.timelimitreached )
            continue;

        if ( isdefined( level.timelimitoverride ) && level.timelimitoverride )
            continue;

        if ( !level.endgameontimelimit )
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

// Émet match_ending_soon, puis very_soon (dernière manche) ou vox (manche
// intermédiaire), une seule fois chacun par manche. Entrée : temps restant en ms.
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

        if ( is_final_round() )
            level notify( "match_ending_very_soon" );
        else
            level notify( "match_ending_vox" );
    }
}

// Dernière manche, ou partie en une manche : test sans effet de bord, au
// contraire de waslastround().
is_final_round()
{
    if ( maps\mp\_utility::isoneround() )
        return 1;

    return maps\mp\_utility::islastround();
}

// ---------------------------------------------------------------------------
// Limites de score
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

    if ( !isplayer( self ) || !isdefined( self.pointstowin ) )
        return;

    if ( self.pointstowin >= level.scorelimit )
        [[ level.onscorelimit ]]();
}

checkteamscorelimitsoon( team )
{
    if ( level.gameended || level.scorelimit <= 0 || !level.endgameonscorelimit )
        return;

    timeleft = maps\mp\gametypes\_globallogic_utils::getestimatedtimeuntilscorelimit( team );
    check_score_ending_soon( timeleft );
}

checkplayerscorelimitsoon()
{
    if ( level.gameended || level.scorelimit <= 0 || !level.endgameonscorelimit )
        return;

    // Équipe indéfinie : l'estimation lit le score du joueur self.
    timeleft = self maps\mp\gametypes\_globallogic_utils::getestimatedtimeuntilscorelimit( undefined );
    check_score_ending_soon( timeleft );
}

// L'estimation est en minutes (999999 si le rythme est nul).
check_score_ending_soon( minutes )
{
    if ( !isdefined( minutes ) || minutes >= 1 )
        return;

    check_match_ending_soon( int( minutes * 60000 ) );
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
// une vie.
has_life_left( player )
{
    // Vies illimitées : toujours une vie.
    if ( level.numlives == 0 )
        return 1;

    if ( !isdefined( player.pers ) || !isdefined( player.pers["lives"] ) )
        return 1;

    return player.pers["lives"] > 0;
}

// Les comptes sont à jour dès le retour d'updateteamstatus ; les crochets du
// mode, eux, ne partent qu'une fois par image, sur l'état final (deux morts dans
// la même image ne donnent qu'un passage).
dispatch_team_events()
{
    level notify( "dispatch_team_events" );
    level endon( "dispatch_team_events" );
    level endon( "game_ended" );

    waittillframeend;

    if ( level.gameended || level.ingraceperiod )
        return;

    if ( level.teambased )
        dispatch_team_based_events();
    else
        dispatch_ffa_events();

    check_forfeit();
}

dispatch_team_based_events()
{
    foreach ( team in level.teams )
    {
        if ( level.alivecount[team] != level.prevalivecount[team] )
        {
            level.prevalivecount[team] = level.alivecount[team];
            [[ level.onalivecountchange ]]( team );
        }
    }

    // Équipe morte et dernier survivant : modes à vies limitées seulement.
    if ( level.numlives == 0 || !all_teams_existed() )
        return;

    dead = [];

    foreach ( team in level.teams )
    {
        isdead = level.playercount[team] > 0 && level.alivecount[team] == 0 && level.teamliveswaiting[team] == 0;

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

    if ( level.gameended )
        return;

    if ( level.teams.size > 2 )
        dispatch_last_team_alive();

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

dispatch_last_team_alive()
{
    survivors = [];

    foreach ( team in level.teams )
    {
        if ( level.alivecount[team] > 0 )
            survivors[survivors.size] = team;
    }

    if ( survivors.size != 1 )
    {
        level.lastteamalivesent = 0;
        return;
    }

    if ( !level.lastteamalivesent )
    {
        level.lastteamalivesent = 1;
        [[ level.onlastteamaliveevent ]]( survivors[0] );
    }
}

dispatch_ffa_events()
{
    if ( level.numlives == 0 )
        return;

    if ( level.activeplayers.size != 1 || totalplayercount() < 2 )
    {
        level.ffaoneleftsent = 0;
        return;
    }

    if ( !level.ffaoneleftsent )
    {
        level.ffaoneleftsent = 1;
        [[ level.ononeleftevent ]]( level.activeplayers[0].team );
    }
}

// Forfait.
check_forfeit()
{
    if ( level.gameended || level.ingraceperiod || is_pregame() )
        return;

    if ( level.teambased )
    {
        withplayers = [];
        emptied = 0;

        foreach ( team in level.teams )
        {
            if ( level.playercount[team] > 0 )
                withplayers[withplayers.size] = team;
            else if ( level.everexisted[team] )
                emptied++;
        }

        if ( withplayers.size == 1 && emptied > 0 )
            start_forfeit( withplayers[0] );
        else
            abort_forfeit();

        return;
    }

    if ( level.maxplayercount >= 2 && totalplayercount() == 1 )
        start_forfeit( undefined );
    else
        abort_forfeit();
}

start_forfeit( team )
{
    if ( level.forfeitpending )
        return;

    level.forfeitpending = 1;
    level thread [[ level.onforfeit ]]( team );
}

abort_forfeit()
{
    if ( !level.forfeitpending )
        return;

    level.forfeitpending = 0;
    level notify( "abort forfeit" );
}

totalplayercount()
{
    count = 0;

    foreach ( player in level.players )
    {
        if ( is_active_player( player ) )
            count++;
    }

    return count;
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

    // Tri par insertion, du meilleur au moins bon.
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

istopscoringplayer( player )
{
    if ( !isdefined( player ) )
        return 0;

    best = score_of( player.score );

    foreach ( other in level.players )
    {
        if ( isdefined( other ) && score_of( other.score ) > best )
            return 0;
    }

    return 1;
}

// ---------------------------------------------------------------------------
// Vainqueurs
// ---------------------------------------------------------------------------

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

determineteamwinnerbyteamscore()
{
    winner = "tie";
    best = undefined;

    foreach ( team in level.teams )
    {
        value = [[ level._getteamscore ]]( team );

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

// Joueur en tête, ou "tie" si la tête est partagée.
top_player()
{
    updateplacement();
    ranked = level.placement["all"];

    if ( ranked.size == 0 )
        return "tie";

    if ( ranked.size > 1 && !ranks_before( ranked[0], ranked[1] ) )
        return "tie";

    return ranked[0];
}

current_winner()
{
    if ( level.teambased )
        return determineteamwinnerbyteamscore();

    return top_player();
}

// ---------------------------------------------------------------------------
// Fin de manche et de partie
// ---------------------------------------------------------------------------

forceend()
{
    if ( level.gameended || level.forcedend )
        return;

    level.forcedend = 1;
    level.hostforcedend = 1;
    endgame( current_winner(), &"MP_HOST_ENDED_GAME" );
}

killserverpc()
{
    if ( level.gameended || level.forcedend )
        return;

    level.forcedend = 1;
    level.hostforcedend = 1;
    level.skipendscreens = 1;
    endgame( current_winner(), &"MP_HOST_ENDED_GAME" );
}

listenforgameend()
{
    // _menus traite déjà les demandes de fin de l'hôte ; on se contente de
    // s'arrêter proprement.
    self endon( "disconnect" );
    level waittill( "game_ended" );
}

endgame( winner, endreasontext )
{
    if ( level.gameended )
        return;

    level.gameended = 1;

    // La suite tourne dans son propre fil : l'appelant porte souvent un
    // endon( "game_ended" ) qui l'arrêtera dès la notification ci-dessous.
    level thread run_endgame( winner, endreasontext );
}

run_endgame( winner, endreasontext )
{
    if ( !isdefined( endreasontext ) )
        endreasontext = "";

    level.gameendtime = gettime();
    game["state"] = "postgame";
    stop_timer();
    level notify( "game_ended" );

    maps\mp\gametypes\_globallogic_audio::flushdialog();
    publish_end_reason( endreasontext );

    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player stop_playing();
    }

    record_round( winner );

    if ( maps\mp\_utility::waslastround() )
        end_match( winner, endreasontext );
    else
        next_round( winner, endreasontext );
}

// Texte de fin pour l'interface : le dvar est d'abord déclaré côté serveur,
// puis rempli.
publish_end_reason( text )
{
    dvar = "ui_text_endreason";
    makedvarserverinfo( dvar, text );
    setdvar( dvar, text );
}

// Fige le joueur self et masque son interface de jeu jusqu'à sa prochaine
// apparition.
stop_playing()
{
    self thread maps\mp\gametypes\_globallogic_player::freezeplayerforroundend();
    self setclientuivisibilityflag( "hud_visible", false );
}

stop_timer()
{
    if ( !level.timerstopped )
    {
        level.timerstopped = 1;
        level.timerpausetime = gettime();
    }

    game["timepassed"] += round_elapsed_ms();
    setgameendtime( 0 );
}

record_round( winner )
{
    game["roundsplayed"]++;

    if ( level.teambased )
    {
        if ( is_team_or_tie( winner ) )
            game["roundswon"][winner]++;

        foreach ( team in level.teams )
            game["lastroundscore"][team] = [[ level._getteamscore ]]( team );
    }

    [[ level.onendgame ]]( winner );
    maps\mp\_challenges::roundend( winner );
    maps\mp\gametypes\_globallogic_utils::executepostroundevents();
}

is_team_or_tie( winner )
{
    if ( !isdefined( winner ) || isplayer( winner ) )
        return 0;

    return winner == "tie" || isdefined( level.teams[winner] );
}

next_round( winner, endreasontext )
{
    switching = level.roundswitch > 0 && game["roundsplayed"] % level.roundswitch == 0;

    if ( switching )
        [[ level.onroundswitch ]]();

    overtime = isdefined( level.nextroundisovertime ) && level.nextroundisovertime;

    if ( overtime )
    {
        if ( isdefined( game["overtime_round"] ) )
            game["overtime_round"]++;
        else
            game["overtime_round"] = 1;
    }

    shown = "roundend";
    text = endreasontext;

    if ( switching )
    {
        shown = "halftime";

        if ( isdefined( level.halftimetype ) )
            shown = level.halftimetype;

        text = &"MP_SWITCHING_SIDES_CAPS";
    }
    else if ( overtime )
    {
        shown = "overtime";
    }
    else if ( level.displayroundendtext )
    {
        shown = winner;
    }

    level.postroundtime = 5;
    show_outcome( shown, 1, text );
    level thread maps\mp\gametypes\_globallogic_audio::announceroundwinner( winner, 1 );

    if ( switching )
    {
        dialog = maps\mp\gametypes\_globallogic_audio::getroundswitchdialog( shown );
        level thread maps\mp\gametypes\_globallogic_audio::leaderdialog( dialog );
    }

    if ( isdefined( level.endgamefunction ) )
        level thread [[ level.endgamefunction ]]();

    wait level.postroundtime;

    play_final_killcam( winner );

    if ( level.wagermatch )
        maps\mp\gametypes\_wager::finalizewagerround();

    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player thread maps\mp\gametypes\_globallogic_spawn::spawninterroundintermission();
    }

    wait 4;

    level notify( "round_end_finished" );
    game["roundMillisecondsAlreadyPassed"] = 0;
    map_restart( 1 );
}

// Fin de partie (et killserverpc, sans les écrans).
end_match( winner, endreasontext )
{
    final = winner;

    if ( !level.forcedend && ( level.roundlimit != 1 || game["roundsplayed"] > 1 ) )
    {
        result = [[ level.onroundendgame ]]( winner );

        if ( isdefined( result ) )
            final = result;
    }

    maps\mp\gametypes\_globallogic_score::updatewinlossstats( final );
    maps\mp\gametypes\_globallogic_score::updatematchbonusscores( final );
    maps\mp\gametypes\_globallogic_score::doskillupdate( final );
    level notify( "give_match_bonus" );
    maps\mp\_challenges::gameend( final );

    if ( level.wagermatch )
    {
        maps\mp\gametypes\_wager::calculateplacesbasedonscore();
        maps\mp\gametypes\_wager::finalizewagergame();
    }

    if ( isdefined( level.skipendscreens ) && level.skipendscreens )
    {
        leave_level();
        return;
    }

    level.postroundtime = 8;
    show_outcome( final, 0, endreasontext );
    level thread maps\mp\gametypes\_globallogic_audio::announcegamewinner( final, 1 );

    wait level.postroundtime;
    level notify( "sfade" );

    // Délai seul, sans second argument : takeoff n'attend la killcam finale que
    // si ce second argument manque.
    if ( isdefined( level.preendgamefunction ) )
    {
        secondsbeforeend = 10;
        level thread [[ level.preendgamefunction ]]( secondsbeforeend );
    }

    play_final_killcam( final );

    // Le temps de la bombe (nuketown) ou de la fusée (takeoff).
    if ( isdefined( level.endgamefunction ) )
    {
        level thread [[ level.endgamefunction ]]();
        wait 6;
    }

    level.intermission = 1;

    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player thread [[ level.spawnintermission ]]();
    }

    wait 12;
    leave_level();
}

leave_level()
{
    level notify( "game_end" );
    exitlevel( 0 );
}

// Écran de résultat de chaque joueur. isround : 1 pour une fin de manche.
show_outcome( winner, isround, endreasontext )
{
    if ( level.dontshowendreason )
        endreasontext = "";

    foreach ( player in level.players )
    {
        if ( !isdefined( player ) )
            continue;

        text = endreasontext;

        if ( !isround && level.teambased && isdefined( level.endvictoryreasontext ) && isdefined( level.enddefeatreasontext ) )
        {
            if ( isdefined( player.team ) && is_team_or_tie( winner ) && player.team == winner )
                text = level.endvictoryreasontext;
            else
                text = level.enddefeatreasontext;
        }

        if ( level.wagermatch && !isround )
        {
            if ( level.teambased )
                player thread [[ level.onteamwageroutcomenotify ]]( winner, isround, text );
            else
                player thread [[ level.onwageroutcomenotify ]]( winner, text );
        }
        else if ( level.teambased )
        {
            player thread [[ level.onteamoutcomenotify ]]( winner, isround, text );
        }
        else
        {
            player thread [[ level.onoutcomenotify ]]( winner, isround, text );
        }
    }
}

play_final_killcam( winner )
{
    if ( !isdefined( level.finalkillcam ) || !level.finalkillcam )
        return;

    level.finalkillcam_winner = winner;
    resetoutcomeforallplayers();
    level.infinalkillcam = 1;
    maps\mp\gametypes\_killcam::postroundfinalkillcam();
    level.infinalkillcam = 0;
}

resetoutcomeforallplayers()
{
    // Chaque écran de résultat de _hud_message attend ce signal pour détruire
    // ses éléments.
    foreach ( player in level.players )
    {
        if ( isdefined( player ) )
            player notify( "reset_outcome" );
    }
}

// ---------------------------------------------------------------------------
// Réapparition par vagues
// ---------------------------------------------------------------------------

// _globallogic_spawn fait apparaître les joueurs d'après lastwave et wavedelay :
// ici, on ne fait que marquer chaque vague.
wave_spawn_timer()
{
    level endon( "game_ended" );

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
    // Statistiques non suivies hors ligne ; chemin en ligne inconnu.
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
    // Télémétrie : rien à écrire hors ligne.
}

getkillstreaks( player )
{
    if ( isdefined( player ) && isdefined( player.killstreak ) )
        return player.killstreak;

    return [];
}

gamehistoryplayerkicked()
{
    bbplayermatchend( getgamelength(), "MP_PLAYER_KICKED", 0 );
}

gamehistoryplayerquit()
{
    bbplayermatchend( getgamelength(), "MP_PLAYER_QUIT", 0 );
}

// ---------------------------------------------------------------------------
// Divers
// ---------------------------------------------------------------------------

// Défaut de crochet. Comparé par identité (level.onplayerdamage == ::blank).
blank()
{
}
