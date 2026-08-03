"""Sprintboard's local image-tagging service."""

from .api import PROTOCOL_VERSION, ServiceSettings, create_app

__all__ = ["PROTOCOL_VERSION", "ServiceSettings", "create_app"]
