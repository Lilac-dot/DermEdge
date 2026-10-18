"""
Label harmonization for the skin-tone compression audit (direction 3): maps each
external dataset's own diagnosis names onto this project's Stage B task -- Eczema vs.
the 7 look-alike classes the curated DermNet-style model was trained on (Psoriasis,
Tinea, Candidiasis, Infestations_Bites, Lichen, DrugEruption, Rosacea).

Every mapping is an explicit name -> class dict (no regexes), so exactly which source
labels count as what is reviewable in one place. Any source label not listed here is
dropped, not guessed at.

Eczema definition: follows the curated training set's Eczema class (DermNet's
"Eczema Photos" category), which includes atopic/nummular/dyshidrotic/hand/foot eczema,
infected eczema, lichen simplex chronicus (= neurodermatitis) and prurigo nodularis.
Deliberately EXCLUDED from both classes because they're ambiguous relative to that
definition: allergic/irritant contact dermatitis (DermNet files these in a separate
category), seborrheic dermatitis, "dermatitis, NOS", photodermatitis, neurotic
excoriations / factitial dermatitis, and "eczematized tinea".
"""

ECZEMA = "Eczema"

# SCIN (Ward et al. 2024): names from weighted_skin_condition_label, which already
# merges clinical synonyms (e.g. nummular/dyshidrotic -> "Eczema").
SCIN_MAP = {
    "Eczema": ECZEMA,
    "Infected eczema": ECZEMA,
    "Lichen Simplex Chronicus": ECZEMA,
    "Prurigo nodularis": ECZEMA,
    "Lichenified eczematous dermatitis": ECZEMA,
    "Lichenified eczema": ECZEMA,
    "Acute-on-chronic dyshidrotic eczema of hands": ECZEMA,
    "Acute constitutional eczema": ECZEMA,
    "Crusted eczematous dermatitis": ECZEMA,
    "Psoriasis": "Psoriasis",
    "Inverse psoriasis": "Psoriasis",
    "Tinea": "Tinea",
    "Tinea Versicolor": "Tinea",
    "Candidal intertrigo": "Candidiasis",
    "Candida intertrigo": "Candidiasis",
    "Candida": "Candidiasis",
    "Candida infection of flexural skin": "Candidiasis",
    "Insect Bite": "Infestations_Bites",
    "Scabies": "Infestations_Bites",
    "Infection of tick bite": "Infestations_Bites",
    "Lichen planus/lichenoid eruption": "Lichen",
    "Lichen nitidus": "Lichen",
    "Lichen sclerosus": "Lichen",
    "Lichen striatus": "Lichen",
    "Lichen spinulosus": "Lichen",
    "Drug Rash": "DrugEruption",
    "Localised skin eruption due to drugs and medicaments": "DrugEruption",
    "Rosacea": "Rosacea",
}

# Fitzpatrick17k-C (Abhishek et al. 2025 cleaned release). Has no tinea or candidiasis
# classes at all.
F17K_MAP = {
    "eczema": ECZEMA,
    "dyshidrotic eczema": ECZEMA,
    "neurodermatitis": ECZEMA,
    "lichen simplex": ECZEMA,
    "prurigo nodularis": ECZEMA,
    "psoriasis": "Psoriasis",
    "pustular psoriasis": "Psoriasis",
    "scabies": "Infestations_Bites",
    "tick bite": "Infestations_Bites",
    "lyme disease": "Infestations_Bites",
    "tungiasis": "Infestations_Bites",
    "lichen planus": "Lichen",
    "drug eruption": "DrugEruption",
    "fixed eruptions": "DrugEruption",
    "rosacea": "Rosacea",
}

# DermaCon-IN (Madarkar et al., NeurIPS 2025 D&B). Disease_label column.
DERMACON_MAP = {
    "Atopic Dermatitis": ECZEMA,
    "Eczema": ECZEMA,
    "Infected Eczema": ECZEMA,
    "Disseminated Eczema": ECZEMA,
    "Dry Discoid Eczema": ECZEMA,
    "Ear Eczema": ECZEMA,
    "Chronic eczema with secondary infection": ECZEMA,
    "Crusted eczematous dermatitis": ECZEMA,
    "Lichen Simplex Chronicus": ECZEMA,
    "Lichen simplex": ECZEMA,
    "Prurigo nodularis": ECZEMA,
    "Psoriasis": "Psoriasis",
    "Chronic plaque psoriasis": "Psoriasis",
    "Guttate Psoriasis": "Psoriasis",
    "Psoriasis Vulgaris": "Psoriasis",
    "Palmar psoriasis": "Psoriasis",
    "Inverse psoriasis": "Psoriasis",
    "Pustular psoriasis": "Psoriasis",
    "Tinea": "Tinea",
    "Tinea Capitis": "Tinea",
    "Tinea Corporis": "Tinea",
    "Tinea Cruris": "Tinea",
    "Tinea Faciei": "Tinea",
    "Tinea Manuum": "Tinea",
    "Tinea pedis": "Tinea",
    "Tinea Versicolor": "Tinea",
    "Steroid Modified Tinea": "Tinea",
    "Infected Tinea": "Tinea",
    "Tinea Infected with secondary bacterial Infection": "Tinea",
    "Candidal Balanoposthitis": "Candidiasis",
    "Candidal Intertrigo": "Candidiasis",
    "Candidal Vulvovaginitis": "Candidiasis",
    "Scabies": "Infestations_Bites",
    "Nodular Scabies": "Infestations_Bites",
    "Pustular Scabies": "Infestations_Bites",
    "Scabies Infected": "Infestations_Bites",
    "Insect Bite Reaction": "Infestations_Bites",
    "Tick bite": "Infestations_Bites",
    "Lichen Planus": "Lichen",
    "Lichen Planus pigmentosus": "Lichen",
    "Hypertrophic Lichen Planus": "Lichen",
    "Linear Lichen Planus": "Lichen",
    "Gutted Lichen Planus": "Lichen",
    "Lichen nitidus": "Lichen",
    "Lichen planopilaris": "Lichen",
    "Lichen sclerosus": "Lichen",
    "Lichen striatus": "Lichen",
    "Lichen spinulosus": "Lichen",
    "Lichenoid eruption": "Lichen",
    "Drug Rash": "DrugEruption",
    "Drug eruption": "DrugEruption",
    "Fixed Drug Eruption": "DrugEruption",
    "Bullous Fixed Drug Eruption": "DrugEruption",
    "Rosacea": "Rosacea",
}


def fst_group(fst):
    """Fitzpatrick type (1-6, or None) -> the 3-bin grouping DDI and most dermatology
    fairness work use: I-II / III-IV / V-VI."""
    if fst is None or fst != fst:  # None or NaN
        return None
    fst = int(fst)
    if fst in (1, 2):
        return "I-II"
    if fst in (3, 4):
        return "III-IV"
    if fst in (5, 6):
        return "V-VI"
    return None
