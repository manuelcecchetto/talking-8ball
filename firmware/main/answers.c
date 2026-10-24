// The ball's own answers: used when nobody spoke, or when the server can't be reached.
// Like the original 8-ball they lean yes / maybe / no, in the oracle's voice.
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "app.h"

static const char *const IT[] = {
    "Sì, senza ombra di dubbio", "Le stelle dicono sì", "Sì, ma non oggi", "Contaci",
    "Sì, se lo vuoi davvero", "Il destino annuisce", "Assolutamente sì", "Tutto porta a sì",
    "Sì, e prima di quanto pensi", "La risposta è sì", "Senza dubbio", "Il vento soffia a favore",
    "Forse, chiedi alla luna", "Non ancora", "Riprova dopo il caffè", "Le acque sono torbide",
    "Il tempo dirà", "Concentrati e richiedi", "Meglio non dirtelo ora", "Ci sto ancora pensando",
    "Non contarci", "La mia risposta è no", "Le stelle storcono il naso", "Molto improbabile",
    "Nemmeno per sogno", "No, e lo sai già", "Le carte dicono di no", "Cambia domanda",
    "Il buio sussurra di sì", "Segui il gatto nero", "La strada gira a sinistra",
    "Solo se piove di martedì", "Chiedi a chi ti ama", "La risposta è nel frigo",
};
static const char *const EN[] = {
    "It is certain", "Without a doubt", "Yes, definitely", "You may rely on it",
    "As I see it, yes", "Most likely", "Outlook good", "Signs point to yes",
    "Yes, but not today", "The stars nod", "Reply hazy, try again", "Ask again later",
    "Better not tell you now", "Cannot predict now", "Concentrate and ask again", "Not yet",
    "Don't count on it", "My reply is no", "My sources say no", "Outlook not so good",
    "Very doubtful", "The tide says no", "Only on a Tuesday", "Follow the black cat",
    "Ask the one who loves you", "The answer is in the fridge", "Sleep on it",
};

const char *answers_pick(const char *lang)
{
    bool it = !lang || !lang[0] || strcmp(lang, "en") != 0;
    const char *const *list = it ? IT : EN;
    size_t n = it ? sizeof(IT) / sizeof(*IT) : sizeof(EN) / sizeof(*EN);
    return list[esp_random() % n];
}
