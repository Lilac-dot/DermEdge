"""Shared project paths, resolved relative to this file's location instead of one
user's hardcoded absolute path, so scripts/ can run on any machine that clones the repo."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATASET_DIR = ROOT / "dataset"
WESAD_DIR = DATASET_DIR / "WESAD"
AAUWSS_DIR = DATASET_DIR / "AAUWSS"
SKINDISEASE_DIR = ROOT / "SkinDisease"
ECZEMA_DIR = ROOT / "Eczema"
MODELS_DIR = ROOT / "models"
SCIN_DIR = DATASET_DIR / "SCIN"
SKINDISNET_DIR = ROOT / "SkinDisNet"
