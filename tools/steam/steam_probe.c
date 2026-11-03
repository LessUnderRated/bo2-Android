// steam_probe.c — demande au Steam Mac du joueur qui est connecté, s'il possède BO2, et si Steam délivre les deux
// tickets que le jeu présente à Demonware (ticket d'application chiffré, ticket de session), par SA libsteam_api.dylib
// (livrée dans Steam.app, jamais redistribuée). On n'affiche que des tailles et des codes, jamais un octet de ticket.
// clang -o /tmp/es tools/steam/steam_probe.c ;
// SteamAppId=202990 LIB=".../Steam Helper.app/Contents/MacOS/libsteam_api.dylib" /tmp/es

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
int main(int c, char **v) {
  const char *L = getenv("LIB");
  void *h = dlopen(L, RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  int (*initflat)(char*) = dlsym(h, "SteamAPI_InitFlat");
  void *(*user)(void) = dlsym(h, "SteamAPI_SteamUser_v023");
  void *(*apps)(void) = dlsym(h, "SteamAPI_SteamApps_v009");
  void *(*utils)(void) = dlsym(h, "SteamAPI_SteamUtils_v011");
  uint64_t (*sid)(void*) = dlsym(h, "SteamAPI_ISteamUser_GetSteamID");
  int (*logged)(void*) = dlsym(h, "SteamAPI_ISteamUser_BLoggedOn");
  int (*sub)(void*, uint32_t) = dlsym(h, "SteamAPI_ISteamApps_BIsSubscribedApp");
  uint32_t (*appid)(void*) = dlsym(h, "SteamAPI_ISteamUtils_GetAppID");
  uint64_t (*req)(void*, void*, int) = dlsym(h, "SteamAPI_ISteamUser_RequestEncryptedAppTicket");
  bool (*getenc)(void*, void*, int, uint32_t*) = dlsym(h, "SteamAPI_ISteamUser_GetEncryptedAppTicket");
  uint32_t (*auth)(void*, void*, int, uint32_t*, const void*) = dlsym(h, "SteamAPI_ISteamUser_GetAuthSessionTicket");
  void (*cancel)(void*, uint32_t) = dlsym(h, "SteamAPI_ISteamUser_CancelAuthTicket");
  bool (*done)(void*, uint64_t, bool*) = dlsym(h, "SteamAPI_ISteamUtils_IsAPICallCompleted");
  bool (*result)(void*, uint64_t, void*, int, int, bool*) = dlsym(h, "SteamAPI_ISteamUtils_GetAPICallResult");
  void (*run)(void) = dlsym(h, "SteamAPI_RunCallbacks");
  void (*shut)(void) = dlsym(h, "SteamAPI_Shutdown");
  char err[1024] = {0}; int r = initflat(err);
  printf("SteamAppId=%s InitFlat=%d %s\n", getenv("SteamAppId"), r, err); if (r) return 2;
  void *u = user(), *a = apps(), *t = utils();
  uint64_t id = sid(u);
  printf("connecte=%d steamid_non_nul=%d type_individuel=%d appid_vu=%u abonne_202970=%d abonne_202990=%d abonne_212910=%d\n",
         logged(u), id != 0, (int)((id >> 52) & 0xF) == 1, appid(t), sub(a, 202970), sub(a, 202990), sub(a, 212910));

  // Ticket d'application chiffré (ce que t6zm demande avant Demonware : ISteamUser::RequestEncryptedAppTicket).
  // EncryptedAppTicketResponse_t = { EResult m_eResult } ; k_iCallback = 154 ; EResult 1 = OK.
  uint64_t call = req(u, NULL, 0); bool failed = false; int i = 0;
  while (call && !done(t, call, &failed) && i++ < 200) { run(); usleep(50000); }
  int32_t eres = -1; bool ok = call && result(t, call, &eres, sizeof eres, 154, &failed);
  uint8_t buf[4096]; uint32_t n = 0;
  bool got = ok && eres == 1 && getenc(u, buf, sizeof buf, &n);
  printf("ticket_chiffre: appel=%d attente_ms=%d resultat=%d eresult=%d echec=%d obtenu=%d octets=%u\n",
         call != 0, i * 50, ok, eres, failed, got, n);

  // Ticket de session (ISteamUser::GetAuthSessionTicket), annulé aussitôt.
  uint32_t m = 0, ha = auth(u, buf, sizeof buf, &m, NULL);
  printf("ticket_session: handle_non_nul=%d octets=%u\n", ha != 0, m);
  if (ha) cancel(u, ha);
  shut(); return 0;
}
